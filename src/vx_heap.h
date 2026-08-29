#pragma once

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
  Record
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
struct ObjMap : Obj {
  static constexpr ObjType TYPE_TAG = ObjType::Map;
  std::vector<std::pair<Value, Value>> entries;

  inline ObjMap() : Obj(ObjType::Map) {}
  inline explicit ObjMap(std::vector<std::pair<Value, Value>> kvs)
      : Obj(ObjType::Map), entries(std::move(kvs)) {}

  inline Value get(Value key, Value default_val = Value::nil()) const {
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
        gc_paused_depth(0), vm(nullptr),
        total_bytes_allocated(0), total_objects_allocated(0),
        total_objects_freed(0), gc_count(0), last_gc_freed(0) {}

  ~Heap() {
    free_all();
  }

  inline void set_vm(VM *v) { vm = v; }

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
  inline Value cons(Value car, Value cdr) {
    ObjCons *c = allocate<ObjCons>(car, cdr);
    return Value::from_ptr(c);
  }

  inline Value make_vector(uint32_t size, Value fill = Value::unspecified()) {
    ObjVector *v = allocate<ObjVector>(size, fill);
    return Value::from_ptr(v);
  }

  inline Value make_vector_from(const std::vector<Value> &elems) {
    ObjVector *v = allocate<ObjVector>(static_cast<uint32_t>(elems.size()));
    for (size_t i = 0; i < elems.size(); ++i) v->set(static_cast<uint32_t>(i), elems[i]);
    return Value::from_ptr(v);
  }

  // A "multiple values" bundle from (values a b ...) — an ObjVector like
  // any other, just tagged via the otherwise-unused Obj::flags field so
  // call-with-values can tell it apart from a real vector the producer
  // returned on purpose. (values x) with exactly one argument is *not*
  // wrapped — it returns x directly, per R5RS's "same effect as if x had
  // been returned" — so this constructor only ever fires for 0 or 2+
  // values, keeping the common single-value case a plain, unwrapped Value.
  static constexpr uint16_t FLAG_MULTIVALUE = 1;
  inline Value make_multivalue(const std::vector<Value> &elems) {
    ObjVector *v = allocate<ObjVector>(static_cast<uint32_t>(elems.size()));
    for (size_t i = 0; i < elems.size(); ++i) v->set(static_cast<uint32_t>(i), elems[i]);
    v->flags = FLAG_MULTIVALUE;
    return Value::from_ptr(v);
  }

  // An R7RS error-object from (error message irritant...) / raise's own
  // wrapping — same trick as multivalue above: a flagged ObjVector, not
  // a new heap type. Slot 0 is the message (whatever was given — this
  // dialect's `error` accepts a leading tag symbol as readily as a
  // string, e.g. assert's `(error 'assert "..." 'expr)`, so
  // error-object-message doesn't enforce R7RS's stricter string-only
  // reading), the rest are irritants.
  static constexpr uint16_t FLAG_ERROR_OBJECT = 2;
  inline Value make_error_object(Value message, const std::vector<Value> &irritants) {
    ObjVector *v = allocate<ObjVector>(static_cast<uint32_t>(irritants.size()) + 1);
    v->set(0, message);
    for (size_t i = 0; i < irritants.size(); ++i) v->set(static_cast<uint32_t>(i) + 1, irritants[i]);
    v->flags = FLAG_ERROR_OBJECT;
    return Value::from_ptr(v);
  }

  inline Value make_map(std::vector<std::pair<Value, Value>> entries = {}) {
    ObjMap *m = allocate<ObjMap>(std::move(entries));
    return Value::from_ptr(m);
  }

  inline Value make_string(std::string_view sv) {
    ObjString *s = allocate<ObjString>(sv);
    return Value::from_ptr(s);
  }

  // Wraps std::cin/std::cout — not owned, closing this port is a no-op
  // on the underlying stream (see ObjPort::owns_stream).
  inline Value make_std_port(bool is_input, std::istream *std_in, std::ostream *std_out) {
    ObjPort *p = allocate<ObjPort>(is_input);
    p->in = std_in;
    p->out = std_out;
    return Value::from_ptr(p);
  }

  inline Value make_input_file_port(std::unique_ptr<std::ifstream> f) {
    ObjPort *p = allocate<ObjPort>(true);
    p->owns_stream = true;
    p->in = f.get();
    p->ifs = std::move(f);
    return Value::from_ptr(p);
  }

  inline Value make_output_file_port(std::unique_ptr<std::ofstream> f) {
    ObjPort *p = allocate<ObjPort>(false);
    p->owns_stream = true;
    p->out = f.get();
    p->ofs = std::move(f);
    return Value::from_ptr(p);
  }

  // String ports. Unlike file ports these own no OS resource, so closing
  // is a no-op and forgetting to close leaks nothing — get-output-string
  // stays readable afterwards, which is what callers expect.
  inline Value make_output_string_port() {
    ObjPort *p = allocate<ObjPort>(false);
    p->oss = std::make_unique<std::ostringstream>();
    p->out = p->oss.get();
    return Value::from_ptr(p);
  }

  // An output port over a caller-supplied streambuf. The host provides the
  // buffer (the wasm build's line-buffered JS sink); the port owns it from
  // here on, and behaves like any other output port to every caller.
  inline Value make_custom_output_port(std::unique_ptr<std::streambuf> buf) {
    ObjPort *p = allocate<ObjPort>(false);
    p->owned_buf = std::move(buf);
    p->owned_out = std::make_unique<std::ostream>(p->owned_buf.get());
    p->out = p->owned_out.get();
    return Value::from_ptr(p);
  }

  inline Value make_input_string_port(const std::string &s) {
    ObjPort *p = allocate<ObjPort>(true);
    p->iss = std::make_unique<std::istringstream>(s);
    p->in = p->iss.get();
    return Value::from_ptr(p);
  }

  inline Value make_subr(const char *name, NativeSubrFn fn, uint32_t min_a, uint32_t max_a, uint64_t udata = 0) {
    ObjSubr *subr = allocate<ObjSubr>(name, fn, min_a, max_a, udata);
    return Value::from_ptr(subr);
  }

  inline Value make_closure(std::shared_ptr<BytecodeChunk> chunk, uint32_t arity, bool is_variadic, uint32_t env_size = 0, uint32_t max_locals = 1) {
    ObjClosure *cl = allocate<ObjClosure>(std::move(chunk), arity, is_variadic, env_size, max_locals);
    return Value::from_ptr(cl);
  }

  inline Value make_closure(BytecodeChunk *chunk, uint32_t arity, bool is_variadic, uint32_t env_size = 0, uint32_t max_locals = 1) {
    ObjClosure *cl = allocate<ObjClosure>(chunk, arity, is_variadic, env_size, max_locals);
    return Value::from_ptr(cl);
  }

  inline Value make_future(Fiber *fiber) {
    ObjFuture *fut = allocate<ObjFuture>(fiber);
    return Value::from_ptr(fut);
  }

  // Deliberately takes nullptr and is filled in afterwards: this call can
  // collect, and a Fiber built BEFORE it would be reachable from nothing
  // at the moment the collector ran. Building the object first, then the
  // fiber (which allocates nothing the collector manages), leaves no
  // window.
  inline Value make_record(Value tag, Value name) {
    return Value::from_ptr(allocate<ObjRecord>(tag, name));
  }

  inline Value make_generator(Fiber *fiber) {
    ObjGenerator *g = allocate<ObjGenerator>(fiber);
    // The fiber this will own is invisible to the collector otherwise:
    // it is plain new'd C++ memory, not a heap object. Charge for it, or
    // the threshold counts a 40-byte object and lets 32KB pile up behind
    // it. Same reasoning as make_bytes, same mechanism.
    note_extra_bytes(ObjGenerator::FIBER_BASELINE_BYTES);
    return Value::from_ptr(g);
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
    return Value::from_ptr(b);
  }

  // A growable sink — the emitter's end of things. Seal it to view it.
  inline Value make_byte_sink() {
    return Value::from_ptr(allocate<ObjBytes>(ObjBytes::Residency::Building));
  }

  inline Value make_view(Value bytes, uint32_t offset, uint32_t stride,
                         uint32_t count, ElemType elem) {
    return Value::from_ptr(allocate<ObjView>(bytes, offset, stride, count, elem));
  }

  inline Value make_handle(uint32_t id, uint32_t kind_sym) {
    return Value::from_ptr(allocate<ObjHandle>(id, kind_sym));
  }

  // A future no fiber computes: something outside the VM will settle it.
  inline Value make_external_future() {
    ObjFuture *fut = allocate<ObjFuture>(nullptr);
    fut->external = true;
    return Value::from_ptr(fut);
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
    if (bytes_allocated > gc_threshold && vm && gc_paused_depth == 0) {
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
    }
    std::free(obj);
  }

  Obj *head_obj;
  size_t bytes_allocated;
  size_t gc_threshold;
  size_t min_gc_threshold;
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

// RAII Scope Guard for pausing GC
struct GCGuard {
  Heap &heap;
  explicit GCGuard(Heap &h) : heap(h) { heap.pause_gc(); }
  ~GCGuard() { heap.resume_gc(); }
};

} // namespace vxs
