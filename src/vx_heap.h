#pragma once

#include <unordered_map>
#include "vx_value.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <string_view>
#include <functional>
#include <fstream>
#include <sstream>
#include <iostream>
#include <memory>

namespace vxs {

// Forward declarations
struct VM;
struct Fiber;

enum class ObjType : uint8_t {
  Cons,
  Vector,
  String,
  Symbol,
  Closure,
  Subr,
  Fiber,
  Future,
  Map,
  Upvalue,
  Port,
  Handle,
  Bytes,
  View,
  Generator,
  Record,
  // Not a real object kind. destroy_obj stamps it on a QUARANTINED object
  // under --gc-poison, so any later use falls through every switch on type
  // instead of reading a recycled allocation. Last in the enum so no
  // existing case order changes.
  Poisoned
};

// Base object header for all heap-allocated objects
struct Obj {
  ObjType type;
  bool gc_mark;
  uint16_t flags;
  Obj *next_all; // Intrusive linked list of all heap objects

  inline explicit Obj(ObjType t)
      : type(t), gc_mark(false), flags(0), next_all(nullptr) {}

  template <typename T>
  inline bool is() const {
    return type == T::TYPE_TAG;
  }

  template <typename T>
  inline T *as() {
    assert(is<T>() && "Object type mismatch");
    return static_cast<T *>(this);
  }

  template <typename T>
  inline const T *as() const {
    assert(is<T>() && "Object type mismatch");
    return static_cast<const T *>(this);
  }
};

//-----------------------------------------------------------------------------
// 1. Cons Cell
//-----------------------------------------------------------------------------
struct ObjCons : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Cons;
  Value car;
  Value cdr;

  inline ObjCons(Value a, Value d)
      : Obj(ObjType::Cons), car(a), cdr(d) {}
};

//-----------------------------------------------------------------------------
// 2. Vector
//-----------------------------------------------------------------------------
struct ObjVector : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Vector;
  uint32_t size;
  Value *data;

  inline ObjVector(uint32_t sz, Value fill = Value::unspecified())
      : Obj(ObjType::Vector), size(sz), data(nullptr) {
    if (size > 0) {
      data = static_cast<Value *>(std::malloc(size * sizeof(Value)));
      for (uint32_t i = 0; i < size; ++i) data[i] = fill;
    }
  }

  inline ~ObjVector() {
    if (data) std::free(data);
  }

  inline Value get(uint32_t ix) const {
    assert(ix < size && "Vector index out of bounds");
    return data[ix];
  }

  inline void set(uint32_t ix, Value v) {
    assert(ix < size && "Vector index out of bounds");
    data[ix] = v;
  }
};

//-----------------------------------------------------------------------------
// 3. String
//-----------------------------------------------------------------------------
struct ObjString : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::String;
  uint32_t length;
  char *chars;

  inline ObjString(const char *s, uint32_t len)
      : Obj(ObjType::String), length(len), chars(nullptr) {
    chars = static_cast<char *>(std::malloc(len + 1));
    std::memcpy(chars, s, len);
    chars[len] = '\0';
  }

  inline explicit ObjString(std::string_view sv)
      : ObjString(sv.data(), static_cast<uint32_t>(sv.size())) {}

  inline ~ObjString() {
    if (chars) std::free(chars);
  }

  inline std::string_view view() const {
    return std::string_view(chars, length);
  }
};

//-----------------------------------------------------------------------------
// 4. Symbol
//-----------------------------------------------------------------------------
struct ObjSymbol : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Symbol;
  uint32_t id;
  const char *name; // Points to interned string

  inline ObjSymbol(uint32_t sym_id, const char *s_name)
      : Obj(ObjType::Symbol), id(sym_id), name(s_name) {}
};

//-----------------------------------------------------------------------------
// 5. C++ Native Subr (Primitive Function)
//-----------------------------------------------------------------------------
typedef Value (*NativeSubrFn)(VM &vm, uint32_t argc, Value *args);

struct ObjSubr : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Subr;
  const char *name;
  NativeSubrFn fn;
  uint32_t min_args;
  uint32_t max_args; // UINT32_MAX for variadic
  uint64_t user_data;

  inline ObjSubr(const char *n, NativeSubrFn f, uint32_t min_a, uint32_t max_a, uint64_t udata = 0)
      : Obj(ObjType::Subr), name(n), fn(f), min_args(min_a), max_args(max_a), user_data(udata) {}
};

//-----------------------------------------------------------------------------
// 6. Bytecode Closure
//-----------------------------------------------------------------------------
struct BytecodeChunk {
  std::vector<uint8_t> code;
  std::vector<Value> constants;
  std::vector<uint32_t> lines;
};

struct ObjClosure : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Closure;
  std::shared_ptr<BytecodeChunk> chunk;
  uint32_t arity;
  bool is_variadic;
  uint32_t env_size;
  uint32_t max_locals;
  Value *env; // Captured closure environment

  inline ObjClosure(std::shared_ptr<BytecodeChunk> ch, uint32_t ar, bool var, uint32_t e_sz = 0, uint32_t mx_loc = 1)
      : Obj(ObjType::Closure), chunk(std::move(ch)), arity(ar), is_variadic(var),
        env_size(e_sz), max_locals(mx_loc), env(nullptr) {
    if (env_size > 0) {
      env = static_cast<Value *>(std::malloc(env_size * sizeof(Value)));
      for (uint32_t i = 0; i < env_size; ++i) env[i] = Value::unspecified();
    }
  }

  inline ObjClosure(BytecodeChunk *ch, uint32_t ar, bool var, uint32_t e_sz = 0, uint32_t mx_loc = 1)
      : ObjClosure(std::shared_ptr<BytecodeChunk>(ch), ar, var, e_sz, mx_loc) {}

  inline ~ObjClosure() {
    if (env) std::free(env);
  }
};

//-----------------------------------------------------------------------------
// 7. Future & Fiber Concurrency
//-----------------------------------------------------------------------------
struct ObjFuture : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Future;

  // INVARIANT: `fiber` is either a live fiber still in the scheduler, or
  // nullptr. It is never a stale pointer. The scheduler settles a future
  // and clears this the moment the computing fiber completes (see
  // step_all_active_fibers), because it deletes that fiber immediately
  // afterwards. A future with fiber == nullptr and is_completed == false
  // is legitimate: it is awaiting something outside the VM entirely.
  Fiber *fiber;
  Value result;
  bool is_completed;
  // Set when the computing fiber died with an error: `result` then carries
  // what to report, and touching raises rather than silently yielding a
  // bogus value.
  bool is_error;
  // Settled from OUTSIDE the VM — a JS promise, a timer, a GPU callback.
  // Without this bit a pending external future is indistinguishable from
  // a deadlock: both are "not completed, no fiber computing it". One is a
  // bug, the other is just a slow GPU, and conflating them makes every
  // legitimate async call report a deadlock.
  bool external;

  inline explicit ObjFuture(Fiber *f)
      : Obj(ObjType::Future), fiber(f), result(Value::unspecified()),
        is_completed(false), is_error(false), external(false) {}
};

//-----------------------------------------------------------------------------
// 7b. Generator — a coroutine driven by hand, not by the scheduler
//-----------------------------------------------------------------------------
// The other thing a fiber can be. A future is UNDIRECTED and settles
// ONCE: whoever wants the value blocks, the scheduler runs the fiber
// whenever it likes, and the result is memoised forever. A generator is
// DIRECTED and many-shot: exactly one party resumes it, does so
// deliberately, and gets a different value each time.
//
// Those are different enough that overloading `touch` to do both would
// have had to make a settled future un-settle, so this is its own type
// with its own verb.
//
// OWNERSHIP is the substantive difference from ObjFuture. A generator's
// fiber is NOT in active_fibers — nothing round-robins it, and it makes
// no progress unless someone resumes it. That means nothing else will
// ever free it, so this object owns it outright and the destructor
// deletes it.
//
// A generator abandoned mid-run is therefore collected with its fiber
// suspended, and its pending unwind-protect cleanups do NOT run. That is
// not an oversight: it is the same custodian rule vxs_clear_fibers
// already follows, on the reasoning that teardown should be explicit and
// loud rather than a control transfer scheduled by the collector.
struct ObjGenerator : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Generator;

  // What a fiber costs the allocator, charged to the collector so that
  // abandoning generators actually provokes collections. A SlabStack
  // allocates 32KB the moment it exists, so an ObjGenerator is a ~40-byte
  // object owning a thousand times that — and a GC threshold counting
  // only the 40 lets an unbounded amount of fiber pile up between
  // collections. Measured before this existed: 20,000 abandoned
  // generators peaked at 222MB of RSS across FOUR collections.
  //
  // A CONSTANT, not the fiber's live footprint. It is charged on
  // construction and credited back by object_size() on sweep, so the two
  // must agree exactly or bytes_allocated drifts — and it is unsigned, so
  // drifting downward would wrap. A fiber whose stack grows past one slab
  // is undercharged; the baseline is what matters, since it is what every
  // fiber pays whether it does anything or not.
  static constexpr size_t FIBER_BASELINE_BYTES = 32768;

  Fiber *fiber;        // owned; nullptr once it has finished
  Value result;        // the thunk's return value, once done
  bool done;
  bool is_error;
  // True only while this generator is being resumed. A generator that
  // resumes itself, directly or through a chain, would re-enter a fiber
  // already running on the C++ stack below — the same class of bug
  // is_dispatching exists to stop for futures.
  bool running;

  inline explicit ObjGenerator(Fiber *f)
      : Obj(ObjType::Generator), fiber(f), result(Value::unspecified()),
        done(false), is_error(false), running(false) {}

  ~ObjGenerator();
};

//-----------------------------------------------------------------------------
// Rooted — a Value the collector can see
//-----------------------------------------------------------------------------
// Six rooting bugs in this collector had one shape: take a value out of a
// rooted place into a bare C++ local, then allocate. Two sat under
// comments warning about that exact hazard, and one was written three
// lines below a fix for the same thing in the same function. So the rule
// became a type.
//
// A Rooted registers a temp root at its OWN address on construction and
// drops it on destruction, which means its protection lasts exactly as
// long as the object does. That one sentence covers both uses, and they
// are not two mechanisms:
//
//   Rooted err(heap, ...);        // a NAMED slot: rooted for the scope
//   heap.make_error_object(heap.make_string(msg), {})
//                                 // a TEMPORARY: rooted for the
//                                 // full-expression, which is why the
//                                 // inner allocation survives the outer
//
// Every allocator returns one, so nesting is safe with nothing written at
// the call site. That was the bug in generator_fault, and it needed four
// lines of push/pop to avoid before this existed.
//
// ⚠️ A GUARD CANNOT BE A PARAMETER TYPE, which was the first design tried.
// C++ evaluates arguments before the callee runs and nothing can root them
// during their own evaluation, so `f(make_string(a), make_string(b))` is
// unsafe however `f` declares its parameters. Guarding the RESULT is what
// works.
//
// ⚠️ WHAT IT DOES NOT COVER. Converting to a bare Value discards the
// guard:
//
//   Value v = heap.cons(a, b);   // the temporary dies at the semicolon
//   heap.cons(x, v);             // v is unrooted across this allocation
//
// That is the subr_map bug's shape. Name it `Rooted` instead and the
// protection lasts the scope.
//
// Non-copyable and therefore non-movable: the root holds &v_, so a copy
// would register two roots and a move would leave one pointing at a
// moved-from shell. It also makes std::vector<Rooted> a compile error,
// which matters because reallocating the buffer would invalidate every
// registered pointer. C++17's guaranteed elision for prvalue returns is
// what lets an allocator still `return Rooted(...)` with no move.
//
// The destructor TRUNCATES to its own depth rather than popping, so it
// composes with the raise path: an unwind truncates temp_roots to a depth
// from before this frame existed, and a blind pop afterwards would take
// somebody else's root.
class Heap;

class Rooted {
public:
  explicit Rooted(Heap &h, Value v = Value::nil());
  ~Rooted();

  Rooted(const Rooted &) = delete;
  Rooted &operator=(const Rooted &) = delete;

  Rooted &operator=(Value v) { v_ = v; return *this; }
  operator Value() const { return v_; }
  Value get() const { return v_; }
  Value *slot() { return &v_; }

private:
  Heap &h_;
  Value v_;
  size_t depth_;
};


//-----------------------------------------------------------------------------
// 7c. Record — a nominal type
//-----------------------------------------------------------------------------
// R7RS wants a record type distinct from every other type. The prelude
// first built these as TAGGED VECTORS, which is the classic portable trick
// and is what a shim on someone else's Scheme has to do. We are not a
// shim, and the leaks were real rather than theoretical:
//
//   (vector? p)             was #t, so a vector? branch shadowed point?
//   (vector-set! p 0 'x)    destroyed the type — the tag was public
//   (display p)             printed [(record-type <point>) 3 4]
//
// The second is what decided it. A type you can dismantle with an ordinary
// vector write is not a floor to build on, and records are about to be
// load-bearing.
//
// `tag` is a fresh object per record TYPE, so identity is by eq? and two
// types sharing a name stay distinct. `name` is carried separately rather
// than parsed back out of the tag, so printing needs no cleverness.
struct ObjRecord : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Record;

  Value tag;                    // identity of the record type
  Value name;                   // a symbol, for printing
  std::vector<Value> fields;

  inline ObjRecord(Value t, Value n)
      : Obj(ObjType::Record), tag(t), name(n) {}
};

//-----------------------------------------------------------------------------
// 8. Associative Map (Key-Value Dict)
//-----------------------------------------------------------------------------
// What a lookup yields when the key is absent and the caller supplied no
// default. #f rather than '(), and the reason is Scheme's truthiness: only
// #f is false, so '() is TRUE and (if (map-ref m k) ...) — the obvious
// idiom — took the present branch for an absent key.
//
// This does NOT disambiguate: a key whose stored value IS #f looks the
// same as an absent one, and nothing can fix that but map-has?. What it
// does is make the common shorthand behave the way its shape promises,
// and make the remaining ambiguity the one people already expect from
// every other language's nil-returning lookup.
inline Value map_missing() { return Value::boolean_false(); }

struct ObjMap : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Map;
  std::vector<std::pair<Value, Value>> entries;

  inline ObjMap() : Obj(ObjType::Map) {}
  inline explicit ObjMap(std::vector<std::pair<Value, Value>> kvs)
      : Obj(ObjType::Map), entries(std::move(kvs)) {}

  inline Value get(Value key, Value default_val = map_missing()) const {
    for (const auto &p : entries) {
      if (p.first == key) return p.second;
    }
    return default_val;
  }

  inline void set(Value key, Value val) {
    for (auto &p : entries) {
      if (p.first == key) {
        p.second = val;
        return;
      }
    }
    entries.push_back({key, val});
  }

  inline bool has(Value key) const {
    for (const auto &p : entries) {
      if (p.first == key) return true;
    }
    return false;
  }
};

//-----------------------------------------------------------------------------
// 9. Upvalue Cell (Shared Mutable Box)
//-----------------------------------------------------------------------------
struct ObjUpvalue : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Upvalue;
  Value value;

  inline explicit ObjUpvalue(Value v = Value::unspecified())
      : Obj(ObjType::Upvalue), value(v) {}
};

//-----------------------------------------------------------------------------
// 10. Port (unified input/output — one type, an is_input flag, matching
// how every other port-consuming primitive already just wants "a stream
// to read from or write to" rather than two unrelated representations)
//-----------------------------------------------------------------------------
struct ObjPort : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Port;
  bool is_input;
  bool closed = false;
  bool owns_stream = false; // true for opened files; false for stdin/stdout
  std::unique_ptr<std::ifstream> ifs; // owned storage for a file input port
  std::unique_ptr<std::ofstream> ofs; // owned storage for a file output port
  // Owned storage for a STRING port. A string port is a port like any
  // other — that is the whole point: `display`, `write`, `newline` and any
  // user procedure that takes a port work on it unchanged, with no second
  // API. Accumulating with string-append instead would be O(n^2) copying
  // and a fresh ObjString per step, which is exactly the wrong shape for
  // generating a page of shader source.
  std::unique_ptr<std::ostringstream> oss;
  std::unique_ptr<std::istringstream> iss;
  // Owned storage for a port over an ARBITRARY streambuf — the browser's
  // sink ports, which forward to a JS callback. Declared after oss/iss but
  // before `out` so destruction order tears the ostream down before the
  // buffer it points at. This is what lets the wasm build make stdout a
  // real port instead of overriding `display`: with a stream behind it,
  // every existing port mechanism (explicit port arguments,
  // current-output-port rebinding, with-output-to-string) works unchanged.
  std::unique_ptr<std::streambuf> owned_buf;
  std::unique_ptr<std::ostream> owned_out;
  std::istream *in = nullptr;  // the stream to actually read from
  std::ostream *out = nullptr; // the stream to actually write to

  inline explicit ObjPort(bool input) : Obj(ObjType::Port), is_input(input) {}

  inline void close_port() {
    if (closed) return;
    if (ifs) ifs->close();
    if (ofs) ofs->close();
    // A sink port holds no OS resource, but it may hold a partial line —
    // flush so closing can't silently swallow it.
    if (owned_out) owned_out->flush();
    closed = true;
  }
};

//-----------------------------------------------------------------------------
// 11. Handle — an opaque reference to something living outside the VM
//-----------------------------------------------------------------------------
// A GPUDevice, GPUBuffer or GPUPipeline cannot cross into wasm, so the host
// keeps the object in a table and we hold an integer index into it.
//
// This is a HEAP OBJECT rather than a NaN-boxed integer, and that is the
// whole design: releasing one reference must invalidate every reference,
// which needs shared mutable state. `(let ((b buf)) (release! buf) (use b))`
// has to fail, and would not if a handle were a copied immediate.
//
// Nothing collects these. The GC will never call destroy() on a GPU buffer,
// and a finalizer would be non-deterministic besides — so ownership is
// explicit, `released` is checked on use, and the live count is exposed so
// a leak is something you can watch rather than something you discover.
struct ObjHandle : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Handle;
  uint32_t id;        // index into the host-side table
  uint32_t kind;      // interned symbol: 'gpu-device, 'gpu-buffer, ...
  bool released;

  inline ObjHandle(uint32_t handle_id, uint32_t kind_sym)
      : Obj(ObjType::Handle), id(handle_id), kind(kind_sym), released(false) {}
};

//-----------------------------------------------------------------------------
// 12. Bytes — untyped storage, and typed views over it
//-----------------------------------------------------------------------------
// Storage is PLAIN BYTES, deliberately, not a family of typed-array types.
// That matches both JS (ArrayBuffer owns memory; Float32Array and friends
// are views with no storage of their own) and WebGPU (getMappedRange hands
// back an ArrayBuffer; writeBuffer takes bytes).
//
// The deeper reason is that GPU data is not uniformly typed. A particle is
// vec3<f32> position + f32 weight + u32 id — one buffer, mixed types, with
// padding. Typed-arrays-as-storage forces one element type per buffer, so
// structured data becomes several parallel buffers that must stay
// index-aligned. That is the normal case, not an edge case.
//
// Hence: the element type lives on the VIEW, never on the buffer. Ask what
// the element type of a particle buffer is and there is no honest answer.
struct ObjBytes : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Bytes;

  // Residency is a state machine. Only the two host-side states exist so
  // far; Device (GPU-resident, not host-readable) and Mapped (temporarily
  // readable, valid only until unmap) arrive with the WebGPU binding.
  enum class Residency : uint8_t {
    Building,   // growable, appendable — an emitter's sink
    Sealed      // fixed size, indexable — a buffer you can bind
  };

  std::vector<uint8_t> data;
  Residency residency;

  inline explicit ObjBytes(Residency r)
      : Obj(ObjType::Bytes), residency(r) {}
};

// How a view interprets the bytes it points at. These are the types WGSL
// actually has (f32/i32/u32), plus u8 for raw access and f64 for host-side
// arithmetic that never reaches a shader.
enum class ElemType : uint8_t { U8, I32, U32, F32, F64 };

inline uint32_t elem_size(ElemType t) {
  switch (t) {
    case ElemType::U8:  return 1;
    case ElemType::I32: return 4;
    case ElemType::U32: return 4;
    case ElemType::F32: return 4;
    case ElemType::F64: return 8;
  }
  return 1;
}

// A view is (buffer, byteOffset, stride, element type, count) and owns no
// storage — exactly JS's model. Two views of different types may overlay
// the same bytes, which is what makes a struct-of-arrays or an
// array-of-structs layout expressible without copying: @P at offset 0
// stride 32, @w at offset 12 stride 32, over one buffer.
//
// NOTE for the kernel compiler: at the Scheme level a view is a value, but
// inside a compiled wrangle it MUST be erased — the layout is known
// statically, so (v3x @P) has to become a raw load at base + i*stride, with
// no view object materializing. A view allocated per point per frame would
// simply be the boxed vec3 problem again under a new name.
struct ObjView : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::View;
  Value bytes;         // the ObjBytes this looks into
  uint32_t offset;     // first element's byte offset
  uint32_t stride;     // bytes between consecutive elements
  uint32_t count;      // number of elements
  ElemType elem;

  inline ObjView(Value b, uint32_t off, uint32_t str, uint32_t n, ElemType e)
      : Obj(ObjType::View), bytes(b), offset(off), stride(str), count(n), elem(e) {}
};

//=============================================================================
// Heap & Slab Allocator with Mark-and-Sweep Garbage Collector
//=============================================================================
class Heap {
public:
  Heap()
      : head_obj(nullptr), bytes_allocated(0),
        gc_threshold(512 * 1024), min_gc_threshold(512 * 1024),
        gc_stress(false), gc_every(0), alloc_tick(0), gc_poison(false),
        gc_paused_depth(0), vm(nullptr),
        total_bytes_allocated(0), total_objects_allocated(0),
        total_objects_freed(0), gc_count(0), last_gc_freed(0) {}

  ~Heap() {
    free_all();
  }

  inline void set_vm(VM *v) { vm = v; }

  // GC STRESS: collect before EVERY allocation.
  //
  // The threshold knob is not enough to shake out rooting bugs, and the
  // reason is in collect_garbage: afterwards the threshold becomes
  // max(min_gc_threshold, bytes_allocated * 2), so once the heap is large
  // a low floor stops mattering and collections go back to being rare.
  // That is what made one such bug fire once every 1621 iterations and
  // move whenever unrelated code changed -- including instrumentation,
  // which made bisecting it self-defeating.
  //
  // Collecting unconditionally makes the whole class DETERMINISTIC: any
  // value live across an allocation and reachable from nothing the
  // collector can see dies on its first allocation rather than its
  // thousandth. Note where the collection lands relative to the
  // constructor -- allocate() collects BEFORE building the object, so the
  // arguments a caller passed by value (cons's car and cdr, say) are
  // exactly the values under test.
  //
  // O(heap) per allocation, so this is a test mode and nothing else.
  //--- ephemeral GC roots -------------------------------------------------
  // These live on the HEAP rather than the VM, so that Rooted
  // below can push and pop them INLINE. They used to sit on the VM, which
  // is an incomplete type here -- and an allocator returning a guard whose
  // constructor cannot be inlined would put a function call on every cons.
  // The VM keeps forwarding methods, so no call site changed.
  //
  // RAW POINTERS INTO C++ STACK FRAMES: anything that unwinds without
  // running the matching pop leaves the collector holding the address of a
  // dead local, which is what truncate_temp_roots is for. See the VM's
  // note at its forwarders.
  std::vector<Value *> temp_roots;
  std::vector<Obj **> temp_obj_roots;

  inline void push_temp_root(Value *v) { temp_roots.push_back(v); }
  inline void pop_temp_root() { if (!temp_roots.empty()) temp_roots.pop_back(); }
  inline void truncate_temp_roots(size_t n) {
    if (temp_roots.size() > n) temp_roots.resize(n);
  }
  inline void push_temp_obj_root(Obj **o) { temp_obj_roots.push_back(o); }
  inline void pop_temp_obj_root() {
    if (!temp_obj_roots.empty()) temp_obj_roots.pop_back();
  }
  inline void truncate_temp_obj_roots(size_t n) {
    if (temp_obj_roots.size() > n) temp_obj_roots.resize(n);
  }

  inline void set_gc_stress(bool on) { gc_stress = on; }
  inline bool is_gc_stress() const { return gc_stress; }

  // Collect every Nth allocation. --gc-stress is this with N = 1, and the
  // reason to want N > 1 is cost: stress is O(heap) per allocation, which
  // is fine for a five-line repro and about seven minutes for the suite --
  // far too slow to run UNDER another instrument. At N = 100 the suite
  // takes seconds while still collecting orders of magnitude more often
  // than the threshold ever will, which is what makes a sanitizer run
  // possible at all.
  inline void set_gc_every(size_t n) { gc_every = n; }
  inline void set_gc_poison(bool on) { gc_poison = on; }
  inline bool is_gc_poison() const { return gc_poison; }

  // "" when the pointer is not a quarantined object; otherwise what it was
  // and which collection took it.
  // Set by the VM before collecting, so a poison record can say what the
  // root set looked like at the moment of the free.
  std::function<std::string()> poison_context = [] { return std::string(); };

  // WRITE-AFTER-FREE DETECTOR. A read of a quarantined object is caught by
  // its Poisoned type; a WRITE is not -- it just scribbles on a corpse,
  // and with real freeing it scribbles on malloc's freelist instead,
  // which surfaces as an unattributable trap inside libsystem_malloc.
  // Call this immediately before writing through a pointer whose owner
  // might already be gone.
  bool warn_if_dead(const Obj *obj, const char *where) const {
    if (!gc_poison || !obj) return false;
    std::string p = poison_of(obj);
    if (p.empty()) return false;
    std::fprintf(stderr, "[gc] WRITE-AFTER-FREE at %s: %s\n", where, p.c_str());
    return true;
  }

  std::string poison_of(const Obj *obj) const {
    auto it = poison_log.find(obj);
    if (it == poison_log.end()) return "";
    std::string ctx;
    auto c = poison_ctx.find(obj);
    if (c != poison_ctx.end()) ctx = " [" + c->second + "]";
    static const char *names[] = {
      "Cons","Vector","String","Symbol","Closure","Subr","Fiber","Future",
      "Map","Upvalue","Port","Handle","Bytes","View","Generator","Record","Poisoned"};
    unsigned t = static_cast<unsigned>(it->second.first);
    return std::string("a ") + (t < 17 ? names[t] : "?") +
           " freed by collection " + std::to_string(it->second.second) + ctx;
  }


  inline void pause_gc() { ++gc_paused_depth; }
  inline void resume_gc() {
    if (gc_paused_depth > 0) --gc_paused_depth;
  }
  inline bool is_gc_paused() const { return gc_paused_depth > 0; }

  // Marking helpers
  inline void mark_value(Value v) {
    if (v.is_ptr()) {
      mark_obj(v.as_ptr<Obj>());
    }
  }

  inline void mark_obj(Obj *obj) {
    if (!obj || obj->gc_mark) return;
    obj->gc_mark = true;
    gray_stack.push_back(obj);
  }

  void mark_fiber(Fiber *f);
  void blacken_obj(Obj *obj);
  void collect_garbage();
  size_t sweep();

  // Allocation helpers
  // Returns Rooted, not Value: see Rooted above. The guard is what makes
  // a nested cons safe, and it measured as free -- 10M conses, 0.76s
  // either way, because a malloc per cell dominates a vector push by ~20x.
  [[nodiscard]] inline Rooted cons(Value car, Value cdr);

  [[nodiscard]] inline Rooted make_vector(uint32_t size, Value fill = Value::unspecified()) {
    ObjVector *v = allocate<ObjVector>(size, fill);
    return Rooted(*this, Value::from_ptr(v));
  }

  [[nodiscard]] inline Rooted make_vector_from(const std::vector<Value> &elems) {
    ObjVector *v = allocate<ObjVector>(static_cast<uint32_t>(elems.size()));
    for (size_t i = 0; i < elems.size(); ++i) v->set(static_cast<uint32_t>(i), elems[i]);
    return Rooted(*this, Value::from_ptr(v));
  }

  // A "multiple values" bundle from (values a b ...) — an ObjVector like
  // any other, just tagged via the otherwise-unused Obj::flags field so
  // call-with-values can tell it apart from a real vector the producer
  // returned on purpose. (values x) with exactly one argument is *not*
  // wrapped — it returns x directly, per R5RS's "same effect as if x had
  // been returned" — so this constructor only ever fires for 0 or 2+
  // values, keeping the common single-value case a plain, unwrapped Value.
  static constexpr uint16_t FLAG_MULTIVALUE = 1;
  [[nodiscard]] inline Rooted make_multivalue(const std::vector<Value> &elems) {
    ObjVector *v = allocate<ObjVector>(static_cast<uint32_t>(elems.size()));
    for (size_t i = 0; i < elems.size(); ++i) v->set(static_cast<uint32_t>(i), elems[i]);
    v->flags = FLAG_MULTIVALUE;
    return Rooted(*this, Value::from_ptr(v));
  }

  // An R7RS error-object from (error message irritant...) / raise's own
  // wrapping — same trick as multivalue above: a flagged ObjVector, not
  // a new heap type. Slot 0 is the message (whatever was given — this
  // dialect's `error` accepts a leading tag symbol as readily as a
  // string, e.g. assert's `(error 'assert "..." 'expr)`, so
  // error-object-message doesn't enforce R7RS's stricter string-only
  // reading), the rest are irritants.
  static constexpr uint16_t FLAG_ERROR_OBJECT = 2;
  [[nodiscard]] inline Rooted make_error_object(Value message, const std::vector<Value> &irritants) {
    ObjVector *v = allocate<ObjVector>(static_cast<uint32_t>(irritants.size()) + 1);
    v->set(0, message);
    for (size_t i = 0; i < irritants.size(); ++i) v->set(static_cast<uint32_t>(i) + 1, irritants[i]);
    v->flags = FLAG_ERROR_OBJECT;
    return Rooted(*this, Value::from_ptr(v));
  }

  [[nodiscard]] inline Rooted make_map(std::vector<std::pair<Value, Value>> entries = {}) {
    ObjMap *m = allocate<ObjMap>(std::move(entries));
    return Rooted(*this, Value::from_ptr(m));
  }

  [[nodiscard]] inline Rooted make_string(std::string_view sv) {
    ObjString *s = allocate<ObjString>(sv);
    return Rooted(*this, Value::from_ptr(s));
  }

  // Wraps std::cin/std::cout — not owned, closing this port is a no-op
  // on the underlying stream (see ObjPort::owns_stream).
  [[nodiscard]] inline Rooted make_std_port(bool is_input, std::istream *std_in, std::ostream *std_out) {
    ObjPort *p = allocate<ObjPort>(is_input);
    p->in = std_in;
    p->out = std_out;
    return Rooted(*this, Value::from_ptr(p));
  }

  [[nodiscard]] inline Rooted make_input_file_port(std::unique_ptr<std::ifstream> f) {
    ObjPort *p = allocate<ObjPort>(true);
    p->owns_stream = true;
    p->in = f.get();
    p->ifs = std::move(f);
    return Rooted(*this, Value::from_ptr(p));
  }

  [[nodiscard]] inline Rooted make_output_file_port(std::unique_ptr<std::ofstream> f) {
    ObjPort *p = allocate<ObjPort>(false);
    p->owns_stream = true;
    p->out = f.get();
    p->ofs = std::move(f);
    return Rooted(*this, Value::from_ptr(p));
  }

  // String ports. Unlike file ports these own no OS resource, so closing
  // is a no-op and forgetting to close leaks nothing — get-output-string
  // stays readable afterwards, which is what callers expect.
  [[nodiscard]] inline Rooted make_output_string_port() {
    ObjPort *p = allocate<ObjPort>(false);
    p->oss = std::make_unique<std::ostringstream>();
    p->out = p->oss.get();
    return Rooted(*this, Value::from_ptr(p));
  }

  // An output port over a caller-supplied streambuf. The host provides the
  // buffer (the wasm build's line-buffered JS sink); the port owns it from
  // here on, and behaves like any other output port to every caller.
  [[nodiscard]] inline Rooted make_custom_output_port(std::unique_ptr<std::streambuf> buf) {
    ObjPort *p = allocate<ObjPort>(false);
    p->owned_buf = std::move(buf);
    p->owned_out = std::make_unique<std::ostream>(p->owned_buf.get());
    p->out = p->owned_out.get();
    return Rooted(*this, Value::from_ptr(p));
  }

  [[nodiscard]] inline Rooted make_input_string_port(const std::string &s) {
    ObjPort *p = allocate<ObjPort>(true);
    p->iss = std::make_unique<std::istringstream>(s);
    p->in = p->iss.get();
    return Rooted(*this, Value::from_ptr(p));
  }

  [[nodiscard]] inline Rooted make_subr(const char *name, NativeSubrFn fn, uint32_t min_a, uint32_t max_a, uint64_t udata = 0) {
    ObjSubr *subr = allocate<ObjSubr>(name, fn, min_a, max_a, udata);
    return Rooted(*this, Value::from_ptr(subr));
  }

  [[nodiscard]] inline Rooted make_closure(std::shared_ptr<BytecodeChunk> chunk, uint32_t arity, bool is_variadic, uint32_t env_size = 0, uint32_t max_locals = 1) {
    ObjClosure *cl = allocate<ObjClosure>(std::move(chunk), arity, is_variadic, env_size, max_locals);
    return Rooted(*this, Value::from_ptr(cl));
  }

  [[nodiscard]] inline Rooted make_closure(BytecodeChunk *chunk, uint32_t arity, bool is_variadic, uint32_t env_size = 0, uint32_t max_locals = 1) {
    ObjClosure *cl = allocate<ObjClosure>(chunk, arity, is_variadic, env_size, max_locals);
    return Rooted(*this, Value::from_ptr(cl));
  }

  [[nodiscard]] inline Rooted make_future(Fiber *fiber) {
    ObjFuture *fut = allocate<ObjFuture>(fiber);
    return Rooted(*this, Value::from_ptr(fut));
  }

  // Deliberately takes nullptr and is filled in afterwards: this call can
  // collect, and a Fiber built BEFORE it would be reachable from nothing
  // at the moment the collector ran. Building the object first, then the
  // fiber (which allocates nothing the collector manages), leaves no
  // window.
  [[nodiscard]] inline Rooted make_record(Value tag, Value name) {
    return Rooted(*this, Value::from_ptr(allocate<ObjRecord>(tag, name)));
  }

  [[nodiscard]] inline Rooted make_generator(Fiber *fiber) {
    ObjGenerator *g = allocate<ObjGenerator>(fiber);
    // The fiber this will own is invisible to the collector otherwise:
    // it is plain new'd C++ memory, not a heap object. Charge for it, or
    // the threshold counts a 40-byte object and lets 32KB pile up behind
    // it. Same reasoning as make_bytes, same mechanism.
    note_extra_bytes(ObjGenerator::FIBER_BASELINE_BYTES);
    return Rooted(*this, Value::from_ptr(g));
  }

  // A sealed buffer of n zeroed bytes — fixed size, ready to be viewed.
  inline Value make_bytes(size_t n) {
    ObjBytes *b = allocate<ObjBytes>(ObjBytes::Residency::Sealed);
    b->data.assign(n, 0);
    // allocate() charged for an EMPTY ObjBytes, because that is what it
    // was. Charge for the storage now that it exists, or a 1MB buffer
    // registers as 1,248 bytes and the allocation-rate instrument reports
    // a number with no relation to what was allocated.
    note_extra_bytes(b->data.capacity());
    return Rooted(*this, Value::from_ptr(b));
  }

  // A growable sink — the emitter's end of things. Seal it to view it.
  [[nodiscard]] inline Rooted make_byte_sink() {
    return Rooted(*this, Value::from_ptr(allocate<ObjBytes>(ObjBytes::Residency::Building)));
  }

  inline Value make_view(Value bytes, uint32_t offset, uint32_t stride,
                         uint32_t count, ElemType elem) {
    return Rooted(*this, Value::from_ptr(allocate<ObjView>(bytes, offset, stride, count, elem)));
  }

  inline Value make_handle(uint32_t id, uint32_t kind_sym) {
    return Rooted(*this, Value::from_ptr(allocate<ObjHandle>(id, kind_sym)));
  }

  // A future no fiber computes: something outside the VM will settle it.
  inline Value make_external_future() {
    ObjFuture *fut = allocate<ObjFuture>(nullptr);
    fut->external = true;
    return Rooted(*this, Value::from_ptr(fut));
  }

  // Fast object accessors
  static inline bool is_cons(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Cons;
  }

  static inline bool is_vector(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Vector;
  }

  static inline bool is_multivalue(Value v) {
    return is_vector(v) && (v.as_ptr<Obj>()->flags & FLAG_MULTIVALUE);
  }

  static inline bool is_error_object(Value v) {
    return is_vector(v) && (v.as_ptr<Obj>()->flags & FLAG_ERROR_OBJECT);
  }

  static inline bool is_map(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Map;
  }

  static inline bool is_string(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::String;
  }

  static inline bool is_closure(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Closure;
  }

  static inline bool is_subr(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Subr;
  }

  static inline bool is_future(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Future;
  }

  static inline bool is_record(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Record;
  }

  static inline bool is_generator(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Generator;
  }

  static inline bool is_upvalue(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Upvalue;
  }

  static inline bool is_port(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Port;
  }

  static inline bool is_handle(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Handle;
  }

  static inline bool is_bytes(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::Bytes;
  }

  static inline bool is_view(Value v) {
    return v.is_ptr() && v.as_ptr<Obj>()->type == ObjType::View;
  }

  static inline Value car(Value v) {
    if (!is_cons(v)) return Value::nil();
    return v.as_ptr<ObjCons>()->car;
  }

  static inline Value cdr(Value v) {
    if (!is_cons(v)) return Value::nil();
    return v.as_ptr<ObjCons>()->cdr;
  }

  static inline void set_car(Value v, Value ncar) {
    if (is_cons(v)) v.as_ptr<ObjCons>()->car = ncar;
  }

  static inline void set_cdr(Value v, Value ncdr) {
    if (is_cons(v)) v.as_ptr<ObjCons>()->cdr = ncdr;
  }

  // Tracking
  // Storage an object took on after allocate() had already charged for it.
  // Only the instantaneous and cumulative totals move; the object count
  // does not, since no new object came into being.
  inline void note_extra_bytes(size_t n) {
    bytes_allocated += n;
    total_bytes_allocated += n;
  }

  size_t get_bytes_allocated() const { return bytes_allocated; }
  size_t get_gc_threshold() const { return gc_threshold; }

  // Observability counters. bytes_allocated/live_objects are instantaneous
  // (what the heap holds now); the total_* pair is cumulative since startup
  // and never decreases, which is what makes allocation *rate* measurable —
  // a live-bytes reading alone cannot distinguish "allocates nothing" from
  // "allocates furiously and collects it all".
  size_t get_total_bytes_allocated() const { return total_bytes_allocated; }
  size_t get_total_objects_allocated() const { return total_objects_allocated; }
  size_t get_total_objects_freed() const { return total_objects_freed; }
  size_t get_live_objects() const {
    return total_objects_allocated - total_objects_freed;
  }
  size_t get_gc_count() const { return gc_count; }
  size_t get_last_gc_freed() const { return last_gc_freed; }
  // Sets both the immediate threshold and the floor collect_garbage()
  // grows back to afterward (see min_gc_threshold below) — otherwise a
  // caller lowering this for e.g. GC-pressure testing would only affect
  // the very first collection before snapping back to the 512KB default.
  void set_gc_threshold(size_t t) { gc_threshold = t; min_gc_threshold = t; }
  size_t get_object_count() const {
    size_t count = 0;
    for (Obj *cur = head_obj; cur; cur = cur->next_all) ++count;
    return count;
  }

  static inline size_t obj_allocated_size(Obj *obj) {
    switch (obj->type) {
      case ObjType::Cons:    return sizeof(ObjCons);
      case ObjType::Vector:  return sizeof(ObjVector) + static_cast<ObjVector*>(obj)->size * sizeof(Value);
      case ObjType::String:  return sizeof(ObjString) + static_cast<ObjString*>(obj)->length + 1;
      case ObjType::Symbol:  return sizeof(ObjSymbol);
      case ObjType::Subr:    return sizeof(ObjSubr);
      case ObjType::Closure: return sizeof(ObjClosure) + static_cast<ObjClosure*>(obj)->env_size * sizeof(Value);
      case ObjType::Fiber:   return sizeof(Obj);
      case ObjType::Future:  return sizeof(ObjFuture);
      case ObjType::Map:     return sizeof(ObjMap) + static_cast<ObjMap*>(obj)->entries.capacity() * sizeof(std::pair<Value, Value>);
      case ObjType::Upvalue: return sizeof(ObjUpvalue);
      case ObjType::Port:    return sizeof(ObjPort);
      case ObjType::Handle:  return sizeof(ObjHandle);
      case ObjType::Bytes:   return sizeof(ObjBytes) + static_cast<ObjBytes*>(obj)->data.capacity();
      case ObjType::View:    return sizeof(ObjView);
      // Must mirror what make_generator charged, exactly — see
      // ObjGenerator::FIBER_BASELINE_BYTES. The charge stands until the
      // object is swept even if resume() already freed the fiber, because
      // it is the sweep that credits it back.
      case ObjType::Generator: return sizeof(ObjGenerator) + ObjGenerator::FIBER_BASELINE_BYTES;
      case ObjType::Record:  return sizeof(ObjRecord) + static_cast<ObjRecord*>(obj)->fields.capacity() * sizeof(Value);
    }
    return sizeof(Obj);
  }

  // Direct allocator
  template <typename T, typename... Args>
  inline T *allocate(Args &&...args) {
    bool due = gc_stress || bytes_allocated > gc_threshold;
    if (!due && gc_every) due = (++alloc_tick % gc_every == 0);
    if (due && vm && gc_paused_depth == 0) {
      collect_garbage();
    }
    void *mem = std::malloc(sizeof(T));
    if (!mem) {
      if (vm && gc_paused_depth == 0) collect_garbage();
      mem = std::malloc(sizeof(T));
      assert(mem && "Out of memory in Heap::allocate");
    }
    T *obj = new (mem) T(std::forward<Args>(args)...);
    obj->next_all = head_obj;
    head_obj = obj;
    // The cumulative counters ride the same cache line as bytes_allocated,
    // which this path already dirties — two increments, no extra size call.
    const size_t sz = obj_allocated_size(obj);
    bytes_allocated += sz;
    total_bytes_allocated += sz;
    ++total_objects_allocated;
    return obj;
  }

private:
  void free_all() {
    Obj *cur = head_obj;
    while (cur) {
      Obj *next = cur->next_all;
      destroy_obj(cur);
      cur = next;
    }
    head_obj = nullptr;
    bytes_allocated = 0;
    gray_stack.clear();
  }

  void destroy_obj(Obj *obj) {
    switch (obj->type) {
      case ObjType::Cons:    static_cast<ObjCons*>(obj)->~ObjCons(); break;
      case ObjType::Vector:  static_cast<ObjVector*>(obj)->~ObjVector(); break;
      case ObjType::String:  static_cast<ObjString*>(obj)->~ObjString(); break;
      case ObjType::Symbol:  static_cast<ObjSymbol*>(obj)->~ObjSymbol(); break;
      case ObjType::Closure: static_cast<ObjClosure*>(obj)->~ObjClosure(); break;
      case ObjType::Subr:    static_cast<ObjSubr*>(obj)->~ObjSubr(); break;
      case ObjType::Fiber:   break;
      case ObjType::Future:  static_cast<ObjFuture*>(obj)->~ObjFuture(); break;
      case ObjType::Map:     static_cast<ObjMap*>(obj)->~ObjMap(); break;
      case ObjType::Upvalue: static_cast<ObjUpvalue*>(obj)->~ObjUpvalue(); break;
      case ObjType::Port:    static_cast<ObjPort*>(obj)->~ObjPort(); break;
      case ObjType::Handle:  break;  // trivially destructible
      case ObjType::Bytes:   static_cast<ObjBytes*>(obj)->~ObjBytes(); break;
      case ObjType::View:    break;  // trivially destructible
      // A generator OWNS its fiber — unlike a future, whose fiber the
      // scheduler owns and reaps. Nothing else can free it, so this must.
      case ObjType::Generator: static_cast<ObjGenerator*>(obj)->~ObjGenerator(); break;
      case ObjType::Record:  static_cast<ObjRecord*>(obj)->~ObjRecord(); break;
      case ObjType::Poisoned: break;   // already quarantined; nothing owns anything
    }
    // QUARANTINE instead of freeing, under --gc-poison.
    //
    // A use-after-free is hard to diagnose because the evidence is gone:
    // malloc hands the memory to somebody else and the dangling pointer
    // reads whatever now lives there, which is why the symptom was
    // "#<unknown>" one time and "#<fiber>" the next. Keeping the header
    // allocated means the pointer still refers to something WE control,
    // so poison_of() below can say what it was and when it died --
    // turning "what died" into "who was holding it".
    //
    // Leaks by construction. A debug mode for a small repro, nothing else.
    if (gc_poison) {
      poison_log[obj] = {obj->type, gc_count};
      // Was the root set even intact when this died? mark_roots reaches a
      // fiber only via active_fibers or the current_fiber -> parent_fiber
      // chain, so a null current_fiber during a collection orphans every
      // fiber that is not active.
      poison_ctx[obj] = poison_context();
      obj->type = ObjType::Poisoned;
      return;
    }
    std::free(obj);
  }


  Obj *head_obj;
  size_t bytes_allocated;
  size_t gc_threshold;
  size_t min_gc_threshold;
  bool gc_stress;
  size_t gc_every;
  size_t alloc_tick;
  bool gc_poison;
  std::unordered_map<const Obj *, std::pair<ObjType, size_t>> poison_log;
  std::unordered_map<const Obj *, std::string> poison_ctx;
  int gc_paused_depth;
  VM *vm;
  std::vector<Obj *> gray_stack;

  // Cumulative observability counters — see the getters above.
  size_t total_bytes_allocated;
  size_t total_objects_allocated;
  size_t total_objects_freed;
  size_t gc_count;
  size_t last_gc_freed;
};

//-----------------------------------------------------------------------------
// Rooted's definitions, now that Heap is complete
//-----------------------------------------------------------------------------

inline Rooted::Rooted(Heap &h, Value v)
    : h_(h), v_(v), depth_(h.temp_roots.size()) {
  h_.push_temp_root(&v_);
}

inline Rooted::~Rooted() { h_.truncate_temp_roots(depth_); }

[[nodiscard]] inline Rooted Heap::cons(Value car, Value cdr) {
  ObjCons *c = allocate<ObjCons>(car, cdr);
  return Rooted(*this, Value::from_ptr(c));
}

// RAII Scope Guard for pausing GC
struct GCGuard {
  Heap &heap;
  explicit GCGuard(Heap &h) : heap(h) { heap.pause_gc(); }
  ~GCGuard() { heap.resume_gc(); }
};

} // namespace vxs
