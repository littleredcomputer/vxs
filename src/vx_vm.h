#pragma once

#include "vx_value.h"
#include "vx_heap.h"
#include <vector>
#include <deque>
#include <map>
#include <memory>
#include <fstream>
#include <unordered_map>
#include <string>
#include <iostream>
#include <sstream>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <chrono>

namespace vxs {

//=============================================================================
// Bytecode Opcodes
//=============================================================================
enum Opcode : uint8_t {
  OP_CONST = 0,
  OP_NIL,
  OP_TRUE,
  OP_FALSE,
  OP_UNSPECIFIED,
  
  OP_POP,

  OP_DEF_GLOBAL,
  OP_GET_GLOBAL,
  OP_SET_GLOBAL,

  OP_GET_LOCAL,
  OP_SET_LOCAL,

  OP_GET_UPVALUE,
  OP_SET_UPVALUE,

  OP_JUMP,
  OP_JUMP_IF_FALSE,

  // NOTE: there are deliberately no arithmetic or comparison opcodes.
  // +, -, <, eq? and friends are ordinary subrs, reached through OP_CALL
  // like any other procedure. Measured, a subr call costs about what a
  // Scheme closure call costs (~18ns against a ~60ns loop iteration), so
  // an opcode would buy a little speed in exchange for making these
  // names keywords — unshadowable, unpassable to map, unredefinable in a
  // test. This VM's bet is a small dispatch loop and a smarter compiler;
  // inlining `let` bought 22% across the whole suite by making the
  // compiler do more, not the loop. A set of such opcodes did once exist
  // here, fully implemented and never emitted by anything.
  OP_CALL,
  OP_TAIL_CALL,
  OP_RETURN,

  OP_CLOSURE,
  OP_FUTURE,
  OP_TOUCH,
  OP_YIELD,

  // Values in both directions across a suspension. OP_YIELD_VALUE pops
  // what the fiber is handing out; OP_PUSH_RESUME pushes what the
  // resumer handed back, and is what makes (yield v) an EXPRESSION
  // rather than a statement. Bare (yield) still compiles to two
  // dispatches — OP_YIELD, OP_PUSH_RESUME — so nothing already written
  // executes more instructions than it did.
  OP_YIELD_VALUE,
  OP_PUSH_RESUME,

  // guard, compiled inline. OP_PUSH_HANDLER carries a u16 offset to the
  // catch target, exactly as the jumps do, and parks a handler record on
  // the fiber; OP_POP_HANDLER drops it when the body finishes normally.
  // Fiber state, not C++ state — which is the whole point, since it is
  // what lets the body (yield) and lets a raise find its handler without
  // a nested dispatch to unwind.
  OP_PUSH_HANDLER,
  OP_POP_HANDLER,

  // unwind-protect support: push a cleanup closure onto the fiber's
  // winder list / pop it back onto the operand stack (to be called).
  // The winder list lives on the Fiber — not the C++ stack — which is
  // what makes (yield) inside a protected body legal: pending cleanups
  // are fiber state and survive suspension like everything else the
  // fiber owns.
  OP_PUSH_WINDER,
  OP_POP_WINDER,

  // Bind a fresh local: pop the operand stack into a slot, overwriting it
  // UNCONDITIONALLY. Deliberately not OP_SET_LOCAL, which writes *through*
  // an ObjUpvalue box if the slot holds one — see the dispatch comment.
  // Appended at the end of the enum on purpose: AOT output (main.cpp)
  // emits raw opcode bytes, so renumbering existing ops would invalidate
  // any previously generated binary.
  OP_INIT_LOCAL,

  // touch, but a failed future yields its error object as a VALUE instead
  // of raising. Exists because a raise needs a handler, `guard` cannot
  // suspend, and the failures that matter — pipeline compilation, mapAsync,
  // device-lost — are exactly the asynchronous ones. Appended at the end of
  // the enum: AOT emits raw opcode bytes, so renumbering breaks binaries.
  OP_TOUCH_VALUE
};

//=============================================================================
// Call Frame
//=============================================================================
struct CallFrame {
  ObjClosure *closure;
  const uint8_t *ip;
  size_t stack_base;
};

//=============================================================================
// SlabStack: a Value stack chunked into fixed-size, individually-owned
// slabs rather than one contiguous, reallocating buffer.
//
// Growth only ever appends a new slab — it never moves a slab that's
// already been handed out. That's what lets call_closure push a nested
// call's frame directly onto the *caller's own* fiber (rather than
// spinning up an isolated Fiber per call, see the VM's call_closure) and
// what lets a native subr hold a raw Value* into the stack for the
// duration of its call: nothing already allocated ever relocates, no
// matter how much gets pushed above it in the meantime.
//
// operator[] is index-based (does a slab lookup), so ordinary single-slot
// access (locals, upvalue capture, etc.) works exactly as it did against
// a flat std::vector<Value> — no caller needs to change. The one thing a
// chunked buffer *can't* give you for free is raw pointer arithmetic over
// a multi-slot contiguous window, which is exactly what a subr's
// `Value *args` parameter is. contiguous_range() answers "does this
// range live in one slab" so the two call sites that hand a subr a raw
// pointer (OP_CALL/OP_TAIL_CALL) can take the fast path (direct pointer,
// the overwhelmingly common case) or fall back to copying that call's
// argc-sized window into a scratch buffer on the rare occasion it
// straddles a slab boundary.
//=============================================================================
class SlabStack {
public:
  static constexpr size_t SLAB_BITS = 12;           // 4096 Values/slab (32 KB)
  static constexpr size_t SLAB_SIZE = size_t(1) << SLAB_BITS;
  static constexpr size_t SLAB_MASK = SLAB_SIZE - 1;

  inline size_t size() const { return sz; }
  inline bool empty() const { return sz == 0; }

  inline Value &operator[](size_t i) {
    return slabs[i >> SLAB_BITS][i & SLAB_MASK];
  }
  inline const Value &operator[](size_t i) const {
    return slabs[i >> SLAB_BITS][i & SLAB_MASK];
  }

  inline Value &back() {
    assert(sz > 0 && "back() on empty SlabStack");
    return (*this)[sz - 1];
  }

  inline void push_back(Value v) {
    ensure_capacity(sz + 1);
    (*this)[sz] = v;
    ++sz;
  }

  inline void pop_back() {
    assert(sz > 0 && "pop_back() on empty SlabStack");
    --sz;
  }

  inline void resize(size_t new_size, Value fill = Value::unspecified()) {
    if (new_size > sz) {
      ensure_capacity(new_size);
      for (size_t i = sz; i < new_size; ++i) (*this)[i] = fill;
    }
    sz = new_size;
  }

  // If [start, start+count) lies within a single slab, points *out at
  // its address and returns true (the common case — direct, no copy).
  // Otherwise returns false and leaves *out untouched; the caller is
  // expected to fall back to a copy.
  inline bool contiguous_range(size_t start, size_t count, Value **out) {
    if (count == 0) {
      static Value empty_sentinel = Value::unspecified();
      *out = &empty_sentinel;
      return true;
    }
    size_t first_slab = start >> SLAB_BITS;
    size_t last_slab = (start + count - 1) >> SLAB_BITS;
    if (first_slab != last_slab) return false;
    ensure_capacity(start + count);
    *out = &slabs[first_slab][start & SLAB_MASK];
    return true;
  }

private:
  std::vector<std::unique_ptr<Value[]>> slabs;
  size_t sz = 0;

  inline void ensure_capacity(size_t needed) {
    while (needed > slabs.size() * SLAB_SIZE) {
      slabs.push_back(std::make_unique<Value[]>(SLAB_SIZE));
    }
  }
};

//=============================================================================
// Fiber (Cooperative Coroutine)
//=============================================================================
// A live `guard`, parked on the fiber that entered it.
//
// The depths are what a raise restores before running the handler. R7RS
// says the clauses evaluate in the dynamic environment of the guard, not
// of the raise, so unwinding FIRST is not an optimisation — it is the
// semantics. They are recorded rather than recomputed because by the time
// a raise is looking for them, the frames that knew are gone.
struct Handler {
  Value handler;            // (lambda (var) (cond clause...))
  const uint8_t *catch_ip;  // where to resume, once unwound
  size_t frame_depth;
  size_t stack_depth;
  size_t winder_depth;
  size_t temp_roots;
  size_t temp_obj_roots;
};

struct Fiber {
  enum class State {
    Ready,
    Running,
    Suspended,
    Completed,
    Error
  };

  State state;
  SlabStack stack;
  std::deque<CallFrame> frames;   // single-element pointer stability, see SlabStack comment
  Value result;
  std::string error_message;

  // Pending unwind-protect cleanups (innermost last). Deliberately fiber
  // state rather than C++-stack state: a protected body may (yield) and
  // the cleanups outlive the suspension; escape continuations and fiber
  // errors run them on the way out (see step_fiber / call/cc's catch).
  // A fiber DISCARDED while suspended (vxs_clear_fibers) does NOT run
  // its winders — that is our custodian moment: teardown is explicit,
  // loud, and not a control transfer. Tying cleanup to fiber liveness
  // would be the guardian/finalizer tar pit by another name.
  std::vector<Value> winders;

  // Live `guard`s, innermost last — the same shape as `winders`, and for
  // the same reason: a guarded body may suspend, and the handler has to
  // outlive that. This is what replaced a C++ try/catch scoped to a
  // nested call_closure, and with it the rule that a guarded body could
  // not yield.
  std::vector<Handler> handlers;

  // Continuation snapshot for call/cc
  std::vector<Value> saved_continuation;
  Fiber *parent_fiber = nullptr;

  // The future this fiber COMPUTES (nil if it wasn't spawned by `future`).
  // ObjFuture holds a raw Fiber* with no reverse link, so when the
  // scheduler reaps a completed fiber it needs a way back to settle the
  // future and clear that pointer — otherwise the future is left holding
  // freed memory and the next `touch` reads it. Held as a Value so
  // mark_fiber traces it; see the invariant note on ObjFuture.
  Value backing_future = Value::nil();

  // The future this fiber is BLOCKED on, nil when runnable. A blocked
  // fiber deliberately stays in active_fibers — it is just a suspended
  // fiber that re-suspends until its future settles. Keeping it in the
  // ring means no second root set for the collector to remember to trace,
  // which is one fewer invariant to keep not-forgetting. The field earns
  // its place by making "blocked" observable: an embedder can report it,
  // and a wait nothing can ever satisfy becomes a diagnosable deadlock
  // rather than a silent hang.
  Value awaited = Value::nil();

  // The two halves of a coroutine handoff, and deliberately fiber state
  // rather than anything the scheduler owns: both must survive an
  // arbitrarily long suspension, and a fiber may sit suspended across
  // any number of collections. Traced by mark_fiber.
  //
  // `yielded` is what (yield v) handed out, read by whoever resumes.
  // `resume_value` is what that resumer passed back, pushed by
  // OP_PUSH_RESUME when the fiber runs again. A round-robin resume
  // leaves it unspecified, which is what every existing (yield) sees
  // and has always seen.
  Value yielded = Value::unspecified();
  Value resume_value = Value::unspecified();

  // Current ports, PER FIBER. They used to be one slot on the VM, which
  // is wrong the moment two fibers redirect at once: with-output-to-string
  // saves the previous port, rebinds, and restores on the way out, and if
  // another fiber rebinds in between it captures the FIRST one's binding
  // as its "previous". Both cleanups then run, in the wrong order, and the
  // global is left pointing at a string port belonging to a finished
  // fiber — after which every display in the VM goes somewhere nobody
  // reads. Every winder fired correctly; the state being restored was
  // simply not the restoring fiber's to own.
  //
  // Captured at spawn from whatever the parent was using, which is what
  // dynamic binding means: a fiber started inside a redirection belongs to
  // it, and one that redirects itself does not leak that outward.
  //
  // Unspecified means "no override, use the VM's" — the state a fiber
  // created before the ports exist is in, and the state all code outside
  // any fiber is in.
  Value out_port = Value::unspecified();
  Value in_port = Value::unspecified();

  inline Fiber()
      : state(State::Ready), result(Value::unspecified()), parent_fiber(nullptr) {}

  inline void push(Value v) {
    stack.push_back(v);
  }

  inline Value pop() {
    assert(!stack.empty() && "Stack underflow");
    Value v = stack.back();
    stack.pop_back();
    return v;
  }

  inline Value &top(size_t offset = 0) {
    assert(stack.size() > offset && "Stack peek out of bounds");
    return stack[stack.size() - 1 - offset];
  }
};

// Escape-only ("bounded") continuations: call-with-current-continuation
// creates a fresh, otherwise-ordinary ObjSubr per invocation, and its own
// identity (a raw pointer, not a separately-tracked token) is what a
// throw of this type carries to identify which invocation it belongs to.
// A C++ exception is what makes this work across arbitrarily nested
// run_dispatch/call_closure/native-subr C++ call frames (map, for-each,
// filter, ...) for free, via ordinary stack unwinding — the same
// property that makes call/cc awkward for anything beyond escape-only
// use is exactly what we're not attempting here. If nothing catches it
// (the escape procedure outlived the call/cc that created it and was
// invoked again later), it surfaces at the top level as a plain runtime
// error instead of crashing, since it derives from std::exception and
// main.cpp's top-level eval_string already catches std::exception.
struct ContinuationEscape : std::runtime_error {
  Obj *target;
  Value value;
  ContinuationEscape(Obj *t, Value v)
      : std::runtime_error("escape continuation invoked outside its dynamic extent"),
        target(t), value(v) {}
};

// raise/guard's exception type. Deliberately carries NO Value payload —
// unlike ContinuationEscape above (whose value field turned out to need
// a manual GC-rooting patch at its one call site once cleanups-during-
// unwind became possible), the raised value instead lives in
// VM::in_flight_raises, a plain vector mark_roots walks directly. A
// std::string is safe to carry here since it holds no Scheme heap
// references — see run_dispatch's file header for the same "no locks
// needed, yield points are the boundaries" ethos: keeping Scheme values
// in structures the GC already knows how to find is the same idea
// applied to exception payloads instead of scheduling.
struct RaiseEscape : std::runtime_error {
  explicit RaiseEscape(const std::string &msg) : std::runtime_error(msg) {}
};

//=============================================================================
// Virtual Machine & Global Environment
//=============================================================================
struct VM {
  Heap heap;
  std::unordered_map<std::string, Value> globals;
  std::unordered_map<std::string, uint32_t> symbol_table;
  std::vector<std::string> symbol_names;
  std::unordered_map<std::string, ObjClosure *> macros;
  uint32_t next_gensym_id = 1;

  // Active concurrent fibers
  std::vector<Fiber *> active_fibers;
  Fiber *current_fiber;

  // The ObjSubr currently executing — NativeSubrFn's signature (VM&,
  // argc, args) doesn't otherwise give a subr body any way to know which
  // ObjSubr instance it's being invoked as. Needed for escape
  // continuations: every call/cc invocation creates its own otherwise-
  // identical escape-procedure ObjSubr, distinguished only by identity,
  // and the shared body needs to read that identity back out to know
  // which continuation it represents. Set/restored around every subr
  // call in run_dispatch (both OP_CALL and OP_TAIL_CALL) and by
  // call_subr below, for the many native subrs (map, for-each, filter,
  // apply, with-output-to-file, ...) that invoke another subr's fn
  // directly rather than going through the bytecode dispatch loop.
  ObjSubr *current_subr = nullptr;

  // Invokes a native subr with current_subr correctly set for the
  // duration — see the field comment above for why this matters
  // (escape continuations identify themselves via current_subr).
  // Centralizes the save/set/restore dance so it can't be forgotten at
  // one of the many direct-invocation call sites.
  inline Value call_subr(ObjSubr *subr, uint32_t argc, Value *args) {
    ObjSubr *prev_subr = current_subr;
    current_subr = subr;
    Value res = subr->fn(*this, argc, args);
    current_subr = prev_subr;
    return res;
  }

  // Ephemeral GC roots
  std::vector<Value *> temp_roots;
  std::vector<Obj **> temp_obj_roots;

  inline void push_temp_root(Value *v) { temp_roots.push_back(v); }
  inline void pop_temp_root() { if (!temp_roots.empty()) temp_roots.pop_back(); }

  // Temp roots are RAW POINTERS INTO C++ STACK FRAMES, so anything that
  // unwinds the C++ stack without running the matching pop_temp_root
  // leaves the collector holding addresses of dead locals. C++ exception
  // unwinding does exactly that: a (raise ...) crossing run_dispatch skips
  // every pop between the throw and the catch.
  //
  // So every site that catches an escape and puts the fiber back together
  // must put these back too. It is the same obligation as restoring the
  // operand stack and the frame list, and it was the one that got missed —
  // silently, because a dangling temp root only matters if a collection
  // happens to land while it is still in the vector.
  inline void truncate_temp_roots(size_t n) {
    if (temp_roots.size() > n) temp_roots.resize(n);
  }
  inline void truncate_temp_obj_roots(size_t n) {
    if (temp_obj_roots.size() > n) temp_obj_roots.resize(n);
  }
  inline void push_temp_obj_root(Obj **o) { temp_obj_roots.push_back(o); }
  inline void pop_temp_obj_root() { if (!temp_obj_roots.empty()) temp_obj_roots.pop_back(); }

  // The value(s) currently mid-raise — see RaiseEscape's comment.
  // `raise` pushes before throwing; whichever %guard catch (or the
  // top-level, for an uncaught raise) is next to see this RaiseEscape
  // pops exactly one entry. A stack rather than a single Value because
  // raises can nest (an unwind-protect cleanup running during an outer
  // raise's unwind can itself raise) — LIFO matches how nested C++
  // exception propagation already unwinds. mark_roots walks it directly.
  std::vector<Value> in_flight_raises;

  // 2D Property table for (put sym prop val) and (get sym prop)
  std::map<std::pair<std::string, std::string>, Value> property_table;

  // I/O Ports — current_in_port/current_out_port are what read/write/
  // display/etc. use when not given an explicit port argument. They
  // default to stdin_port/stdout_port and get temporarily rebound by
  // things like with-output-to-file for the extent of a call.
  Value stdin_port = Value::unspecified();
  Value stdout_port = Value::unspecified();
  Value current_in_port = Value::unspecified();
  Value current_out_port = Value::unspecified();

  // The fiber's own binding when it has one, the VM's otherwise. Every
  // reader of "the current port" must come through these two, or it will
  // see one fiber's redirection from inside another.
  inline Value effective_out_port() const {
    if (current_fiber && !current_fiber->out_port.is_unspecified())
      return current_fiber->out_port;
    return current_out_port;
  }
  inline Value effective_in_port() const {
    if (current_fiber && !current_fiber->in_port.is_unspecified())
      return current_fiber->in_port;
    return current_in_port;
  }
  // Rebinding follows the same rule: inside a fiber it is the fiber's, so
  // it cannot outlive it or be seen by a sibling.
  inline void set_out_port(Value p) {
    if (current_fiber) current_fiber->out_port = p; else current_out_port = p;
  }
  inline void set_in_port(Value p) {
    if (current_fiber) current_fiber->in_port = p; else current_in_port = p;
  }

  inline std::ostream &out_stream() const {
    return *effective_out_port().as_ptr<ObjPort>()->out;
  }
  inline std::istream &in_stream() const {
    return *effective_in_port().as_ptr<ObjPort>()->in;
  }

  // Hook for error diagnostics
  std::string last_error;

  // Symbol ids the COMPILER needs constantly while desugaring — `let`
  // appears eight times in vx_compiler.h, `if` eight, `quote` six.
  // Interning each on every use meant building a std::string and hashing
  // it, per desugar, forever.
  //
  // They live on the VM rather than the Compiler for two reasons. The
  // symbol table is the VM's, so this is where they belong; and a
  // Compiler is constructed for EVERY nested lambda
  // (`Compiler fn_compiler(vm, this)`), so caching there would re-intern
  // twenty names per function compiled instead of once per VM.
  //
  // No post-init needed: symbol_table is a member, so it is already
  // constructed when the constructor body runs — init_primitives() below
  // interns hundreds of names from exactly here.
  struct WellKnown {
    uint32_t s_if, s_let, s_letrec, s_lambda, s_begin, s_quote, s_cond,
             s_else, s_set, s_list, s_append, s_not, s_memv, s_loop,
             s_raise, s_void, s_arrow, s_guard_impl, s_list_to_vector,
             s_call_with_values;
  } sym;

  // lib/prelude.scm is evaluated at the end of init_primitives unless this
  // is false. --no-prelude sets it, which yields the bare kernel: compiler
  // special forms and C++ primitives only. Useful for measuring what the
  // prelude costs at startup, and for catching a kernel that has quietly
  // grown a dependency on something defined in Scheme above it.
  bool prelude_enabled = true;

  // Library sources supplied at RUNTIME, consulted by `load` ahead of the
  // copies compiled into the binary. This exists for watch-mode editing in
  // the browser: lib/*.scm is embedded at build time, so until now a change
  // to a library was invisible there until a rebuild — which bit us once
  // already, when `if` in the kernel language worked natively and did not
  // exist in the page. With an override registered from the served file,
  // saving in an editor is enough.
  //
  // Keyed by BASENAME, matching how embedded_lib_source resolves.
  std::unordered_map<std::string, std::string> lib_overrides;

  explicit VM(bool with_prelude = true) : current_fiber(nullptr) {
    prelude_enabled = with_prelude;
    heap.set_vm(this);
    sym = WellKnown{
        intern("if"),      intern("let"),    intern("letrec"),
        intern("lambda"),  intern("begin"),  intern("quote"),
        intern("cond"),    intern("else"),   intern("set!"),
        intern("list"),    intern("append"), intern("not"),
        intern("memv"),    intern("loop"),   intern("raise"),
        intern("void"),    intern("=>"),     intern("%guard"),
        intern("list->vector"), intern("call-with-values")};
    stdin_port = heap.make_std_port(true, &std::cin, nullptr);
    stdout_port = heap.make_std_port(false, nullptr, &std::cout);
    current_in_port = stdin_port;
    current_out_port = stdout_port;
    init_primitives();
  }

  ~VM() {
    for (Fiber *f : active_fibers) delete f;
    active_fibers.clear();
  }

  void mark_roots(Heap &h);
  inline void collect_garbage() { heap.collect_garbage(); }

  // Runs `closure` to completion on the *current* fiber, pushing its frame
  // directly onto vm.current_fiber's own stack/frames rather than spinning
  // up an isolated Fiber for the call. That's only safe because Fiber's
  // stack/frames now guarantee pointer stability across growth (SlabStack
  // + deque, see vx_vm.h) — nothing pushed by this nested call can
  // invalidate anything the caller (or anything further down the call
  // chain) is holding a pointer into.
  //
  // The payoff beyond avoiding an allocation per call: an error inside
  // the closure sets Fiber::State::Error directly on the one shared
  // fiber object the outer dispatch loop is already checking after every
  // subr call — no separate propagation step, so it can't be forgotten
  // (as it previously was: this call used to run on its own throwaway
  // Fiber, whose error state nothing ever copied back to the caller).
  //
  // Not every call_closure invocation has a fiber to piggyback on, though:
  // defmacro expansion happens inside the compiler (vx_compiler.h), which
  // runs from main.cpp's compile_top_level *before* that top-level form's
  // fiber is ever made current. That path falls back to the old
  // isolated-Fiber behavior — rare (compile-time only, not a hot path),
  // so the extra allocation there doesn't matter.
  Value call_closure(ObjClosure *closure, const std::vector<Value> &args) {
    if (closure->is_variadic) {
      if (args.size() < closure->arity) {
        last_error = "Variadic closure: expected at least " + std::to_string(closure->arity) + " args";
        if (current_fiber) {
          current_fiber->state = Fiber::State::Error;
          current_fiber->error_message = "[VM Error] " + last_error;
        }
        return Value::unspecified();
      }
    } else if (args.size() != closure->arity) {
      last_error = "Closure: expected " + std::to_string(closure->arity) + " args, got " + std::to_string(args.size());
      if (current_fiber) {
        current_fiber->state = Fiber::State::Error;
        current_fiber->error_message = "[VM Error] " + last_error;
      }
      return Value::unspecified();
    }

    // call_closure is a synchronous "run to completion" call with no
    // resume protocol, so anything other than a clean Completed (an
    // explicit (yield) escaping the call, or the instruction budget
    // running out mid-call — e.g. genuine infinite non-tail recursion)
    // is unrecoverable here. That matters more than it did before: this
    // now runs on the caller's own long-lived fiber rather than a
    // throwaway one, so failing to treat it as an error would leave
    // however many frames got pushed permanently stuck on that fiber
    // instead of being discarded with it.
    if (current_fiber) {
      Fiber &f = *current_fiber;
      size_t base_depth = f.frames.size();
      push_closure_frame(f, closure, args);
      // UNBOUNDED, no deadline: this is a synchronous run-to-completion
      // call — nothing else interleaves, so an instruction ceiling here
      // is pure liability (we bumped it once already when boyer.scm
      // legitimately outgrew it). Genuine non-termination is the
      // program's bug, exactly as it would be in any interpreter.
      StepResult res = run_dispatch(f, UNBOUNDED, base_depth);
      if (res != StepResult::Completed || f.state == Fiber::State::Error) {
        if (f.state != Fiber::State::Error) {
          f.state = Fiber::State::Error;
          f.error_message = "[VM Error] call_closure: computation did not "
                            "complete (exceeded instruction budget or "
                            "yielded mid-call)";
        }
        last_error = f.error_message;
        return Value::unspecified();
      }
      return f.result;
    }

    // No active fiber (e.g. compile-time defmacro expansion) — nothing to
    // piggyback on, so fall back to a throwaway one, same as before.
    Fiber scratch;
    push_closure_frame(scratch, closure, args);
    StepResult res = step_fiber(scratch, 100000000);
    if (res != StepResult::Completed || scratch.state == Fiber::State::Error) {
      if (scratch.error_message.empty()) {
        scratch.error_message = "[VM Error] call_closure: computation did not "
                                "complete (exceeded instruction budget or "
                                "yielded mid-call)";
      }
      last_error = scratch.error_message;
      return Value::unspecified();
    }
    return scratch.result;
  }

private:
  // Pushes closure + args (handling the variadic rest-list) onto f and
  // opens its call frame — the part call_closure's two paths share.
  void push_closure_frame(Fiber &f, ObjClosure *closure, const std::vector<Value> &args) {
    f.push(Value::from_ptr(closure));
    if (closure->is_variadic) {
      for (size_t i = 0; i < closure->arity; ++i) f.push(args[i]);
      Value rest_list = Value::nil();
      push_temp_root(&rest_list);
      for (size_t i = args.size(); i > closure->arity; --i) {
        rest_list = heap.cons(args[i - 1], rest_list);
      }
      pop_temp_root();
      f.push(rest_list);
    } else {
      for (Value a : args) f.push(a);
    }
    size_t actual_argc = closure->is_variadic ? (closure->arity + 1) : args.size();
    size_t stack_base = f.stack.size() - actual_argc - 1;
    size_t frame_slots = std::max<size_t>(actual_argc + 1, closure->max_locals);
    f.stack.resize(stack_base + frame_slots, Value::unspecified());
    f.frames.push_back({closure, closure->chunk->code.data(), stack_base});
  }

public:

  // Symbol Interning
  inline uint32_t intern(const std::string &name) {
    auto it = symbol_table.find(name);
    if (it != symbol_table.end()) return it->second;
    uint32_t id = static_cast<uint32_t>(symbol_names.size());
    symbol_names.push_back(name);
    symbol_table[name] = id;
    return id;
  }

  inline const std::string &get_symbol_name(uint32_t id) const {
    assert(id < symbol_names.size() && "Invalid symbol ID");
    return symbol_names[id];
  }

  // Fiber Execution
  //
  // The scheduling model is cooperative, full stop: a fiber runs until
  // its own (yield), completion, or error. That's the invariant Scheme
  // code is entitled to reason with — "nothing else touches shared state
  // between my yields" — and it's what makes this a language without
  // locks: in a cooperative model, the yield points ARE the lock
  // boundaries. Anything that cuts a fiber off at a boundary it didn't
  // choose is preemption, and preemption is reported honestly as
  // StepResult::Preempted, never disguised as a yield. Two opt-in
  // mechanisms can preempt:
  //   - max_instructions: an explicit instruction cap, for debugger-style
  //     "step exactly N and inspect" use on a single fiber. Never a
  //     scheduling device — no sibling fiber runs during such stepping.
  //   - deadline: a wall-clock backstop for embedders that cannot afford
  //     to block (the browser's rAF loop). Overrun is a reported error
  //     condition ("this fiber didn't yield — that's a bug"), not a mode.
  enum class StepResult {
    Completed,
    Yielded,    // the fiber executed OP_YIELD — the only voluntary suspension
    Preempted,  // cut off without asking: instruction cap or deadline hit
    Error
  };

  // "No limit" sentinels. UNBOUNDED makes the instruction-count exit
  // unreachable; NO_DEADLINE short-circuits the clock check entirely.
  static constexpr size_t UNBOUNDED = SIZE_MAX;
  static constexpr std::chrono::steady_clock::time_point NO_DEADLINE =
      std::chrono::steady_clock::time_point::max();

  StepResult step_fiber(Fiber &f, size_t max_instructions = UNBOUNDED,
                        std::chrono::steady_clock::time_point deadline = NO_DEADLINE);

  // Pops and runs f's pending unwind-protect cleanups, innermost first,
  // down to (not including) index down_to. Used on the involuntary exit
  // paths: a fiber entering Error state (step_fiber), and an escape
  // continuation unwinding out of protected extents (call/cc's catch
  // runs the winders the discarded frames left behind; step_fiber runs
  // them for an out-of-extent escape leaving the fiber entirely).
  // Normal exits don't come here — the compiled OP_POP_WINDER sequence
  // runs the cleanup inline. A cleanup that itself escapes or errors
  // abandons the remaining winders (CL's unwind-protect permits the
  // same); an erroring cleanup does not clobber the original error.
  void run_pending_winders(Fiber &f, size_t down_to);

  // Steps every active fiber, each to its own yield/completion/error —
  // except under the shared wall-clock budget, computed once up front:
  // one absolute deadline for the whole call is what actually protects
  // a frame budget (per-fiber slices would just divide the overrun).
  // Returns how many fibers were preempted by the deadline this call, so
  // embedders can surface "a fiber isn't yielding" instead of hiding it.
  //
  // Atomicity is preserved even for a misbehaving fiber: if the deadline
  // cuts one off mid-flight, that fiber is remembered (preempted_fiber)
  // and resumed EXCLUSIVELY on subsequent calls — no sibling steps until
  // it reaches its own yield. A genuinely non-yielding fiber therefore
  // starves its siblings, exactly as it would block forever in the
  // browser's own event loop; the embedder stays responsive and the
  // preempted-count says why. No fiber ever observes another fiber's
  // effects at a non-yield boundary.
  // `exclude`, when set, is skipped entirely. Used by a nested touch: that
  // fiber is mid-dispatch further up the C++ stack, so stepping it again
  // from here would re-enter it recursively.
  size_t step_all_active_fibers(
      size_t instructions_per_fiber = UNBOUNDED,
      std::chrono::milliseconds wall_clock_budget = std::chrono::milliseconds::max(),
      Fiber *exclude = nullptr);

  // Settle the future a finishing fiber was computing, and sever the
  // future's pointer to it. MUST be called immediately before deleting any
  // fiber the scheduler reaps: ObjFuture holds a raw Fiber*, so without
  // this the future is left pointing at freed memory and the next touch
  // reads it (heap-use-after-free, reachable from three lines of Scheme).
  inline void settle_backing_future(Fiber *f) {
    if (!f || !Heap::is_future(f->backing_future)) return;
    ObjFuture *fut = f->backing_future.as_ptr<ObjFuture>();
    if (!fut->is_completed) {
      fut->is_completed = true;
      if (f->state == Fiber::State::Error) {
        fut->is_error = true;
        fut->result = heap.make_string(
            f->error_message.empty() ? "future failed" : f->error_message);
      } else {
        fut->result = f->result;
      }
    }
    fut->fiber = nullptr;   // the invariant: never stale
    f->backing_future = Value::nil();
  }

  // Misuse of these primitives RAISES rather than setting Fiber::State::Error.
  // The older list primitives (car, cdr, ...) set the fiber state, which
  // `guard` cannot catch — a known gap. New code moves toward catchable, so
  // a workbench can report a bad buffer index and carry on rather than
  // losing the fiber. Same choice already made for a failed future.
  [[noreturn]] inline void raise_contract(const std::string &msg) {
    Value m = heap.make_string(msg);
    push_temp_root(&m);
    Value err = heap.make_error_object(m, {});
    pop_temp_root();
    in_flight_raises.push_back(err);
    throw RaiseEscape(msg);
  }

  inline ObjBytes *require_bytes(Value v, const char *who) {
    if (Heap::is_bytes(v)) return v.as_ptr<ObjBytes>();
    raise_contract(std::string(who) +
        ": contract violation, expected a byte buffer, got " + format_value(v));
  }

  inline ObjView *require_view(Value v, const char *who) {
    if (Heap::is_view(v)) return v.as_ptr<ObjView>();
    raise_contract(std::string(who) +
        ": contract violation, expected a view, got " + format_value(v));
  }

  // How to drop the host-side object a handle names. Installed by the
  // embedder — the wasm layer wires this to its JS table; a native CLI
  // has no host objects, so the default no-op is correct there rather
  // than merely harmless. Keeps the VM free of any emscripten dependency.
  std::function<void(uint32_t)> host_handle_releaser;

  inline void release_host_handle(uint32_t id) {
    if (host_handle_releaser) host_handle_releaser(id);
  }

  // Messages from fibers that DIED while the scheduler was stepping them.
  // Without this they were silently discarded: step_all_active_fibers
  // reaps an errored fiber exactly like a completed one, so a fiber that
  // blew up inside a frame simply vanished — no output, no trace, nothing
  // to search for. That cost a full debugging round trip on the first real
  // GPU page. The embedder drains this and reports.
  std::vector<std::string> fiber_errors;

  // How many times a fiber has reached (yield). This — not the browser's
  // frame rate — is how often a render loop actually completed a pass.
  // requestAnimationFrame fires at 60Hz whether the program got anywhere or
  // not, so an FPS reading says only that the browser is painting.
  size_t total_yields = 0;

  // Futures awaiting something outside the VM, keyed by the token handed
  // to whoever will settle them.
  //
  // This table is a GC ROOT, and that is the whole reason it exists rather
  // than passing an ObjFuture* out to JS. Between starting an async call
  // and its callback firing, Scheme may well drop every reference to the
  // future — nothing has touched it yet, so nothing holds it — at which
  // point the collector reclaims it and the callback settles into freed
  // memory. Exactly the use-after-free fixed in the touch rewrite,
  // arriving from the opposite direction. Registration roots it; settling
  // or clearing un-roots it.
  std::unordered_map<uint32_t, Value> pending_externals;
  uint32_t next_external_token = 1;

  // Touching a failed future RAISES rather than killing the fiber, so a
  // rejected promise — a GPU device that never arrived, a shader that
  // failed to compile — is caught by an ordinary (guard ...) like any
  // other condition. Setting Fiber::State::Error instead would make GPU
  // failures the one category of error Scheme code cannot handle, which
  // is precisely backwards for the thing most likely to fail.
  [[noreturn]] inline void raise_failed_future(ObjFuture *fut) {
    Value msg = Heap::is_string(fut->result)
                    ? fut->result
                    : heap.make_string("awaited computation failed");
    Value err = heap.make_error_object(msg, {});
    in_flight_raises.push_back(err);
    throw RaiseEscape(Heap::is_string(msg)
                          ? std::string(msg.as_ptr<ObjString>()->view())
                          : "awaited computation failed");
  }

  // Hand back a token for something outside the VM to settle later.
  inline uint32_t register_external(Value future) {
    uint32_t token = next_external_token++;
    pending_externals[token] = future;
    return token;
  }

  // Settle a pending external future. An UNKNOWN TOKEN IS A QUIET NO-OP,
  // returning false: the callback outlives the thing it was going to
  // resolve all the time — page teardown, vxs_clear_fibers, a cancelled
  // request, a promise landing in a world that no longer has a waiter.
  // That is normal operation, not an error to report.
  inline bool settle_external(uint32_t token, Value result, bool is_error) {
    auto it = pending_externals.find(token);
    if (it == pending_externals.end()) return false;
    Value fut_val = it->second;
    pending_externals.erase(it);          // un-root it
    if (!Heap::is_future(fut_val)) return false;
    ObjFuture *fut = fut_val.as_ptr<ObjFuture>();
    if (fut->is_completed) return false;  // already settled; first wins
    fut->is_completed = true;
    fut->is_error = is_error;
    fut->result = result;
    return true;
  }

  // The deadline-preempted fiber owed an exclusive resume — see above.
  Fiber *preempted_fiber = nullptr;

  // Where the next scheduling round resumes. A round cut short by the
  // wall-clock budget must continue from where it stopped, not restart at
  // index 0 — otherwise the same prefix of active_fibers wins every frame
  // and every fiber past the cutoff is starved forever, however well it
  // yields. (Measured before this existed: with 2000 fibers against an 8ms
  // budget, fiber 0 ran on all 120 ticks and 1649 fibers ran zero times.)
  size_t round_cursor = 0;

  // Take a finished/dead fiber out of active_fibers, settle its future and
  // free it, locating it BY IDENTITY — this scheduler is re-entrant, so a
  // position recorded before a step means nothing after it. See the
  // definition in vx_vm.cpp for the crash this exists to prevent.
  void retire_fiber(Fiber *f, bool record_error);

  // True if f is on the current C++ dispatch stack (current_fiber and its
  // parent_fiber chain). Such a fiber must never be stepped or retired —
  // see the definition in vx_vm.cpp.
  bool is_dispatching(const Fiber *f) const;

  // Shared dispatch loop body. step_fiber calls this with stop_at_depth=0
  // (today's behavior: run until the fiber is genuinely done). call_closure
  // calls it directly with stop_at_depth set to the frame depth it started
  // at, so control returns to the C++ caller once the nested call's own
  // frame(s) unwind back past that point — without touching current_fiber/
  // parent_fiber bookkeeping, since it's already correctly set up by
  // whichever step_fiber invocation is further up the (real) call chain.
  //
  // The deadline is checked only in THIS loop, never inside a nested
  // call_closure dispatch (which always runs UNBOUNDED): call_closure is
  // a synchronous run-to-completion contract — a Preempted result there
  // would strand frames on the shared fiber (see call_closure's comment)
  // — and it also means native primitives (map, for-each, ...) remain
  // atomic units, which is the soundness property we're defending. The
  // cost is that the deadline is best-effort: a single huge primitive
  // call can overshoot it, and the check fires as soon as control
  // returns to the outer loop.
  StepResult run_dispatch(Fiber &f, size_t max_instructions, size_t stop_at_depth,
                          std::chrono::steady_clock::time_point deadline = NO_DEADLINE);

  // Global variables
  inline void def_global(const std::string &name, Value val) {
    globals[name] = val;
  }

  inline Value get_global(const std::string &name) const {
    auto it = globals.find(name);
    if (it != globals.end()) return it->second;
    return Value::unspecified();
  }

  // String formatting / printing
  std::string format_value(Value v) const;
  void display_value(Value v, std::ostream &os) const;

private:
  void init_primitives();
};

} // namespace vxs
