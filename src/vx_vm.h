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
  OP_DUP,

  OP_DEF_GLOBAL,
  OP_GET_GLOBAL,
  OP_SET_GLOBAL,

  OP_GET_LOCAL,
  OP_SET_LOCAL,

  OP_GET_UPVALUE,
  OP_SET_UPVALUE,

  OP_JUMP,
  OP_JUMP_IF_FALSE,
  OP_JUMP_IF_TRUE,

  OP_ADD,
  OP_SUB,
  OP_MUL,
  OP_DIV,
  OP_REM,

  OP_EQ,
  OP_NUM_EQ,
  OP_LT,
  OP_LE,
  OP_GT,
  OP_GE,
  OP_NOT,

  OP_CALL,
  OP_TAIL_CALL,
  OP_RETURN,

  OP_CLOSURE,
  OP_FUTURE,
  OP_TOUCH,
  OP_YIELD
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

  // Continuation snapshot for call/cc
  std::vector<Value> saved_continuation;
  Fiber *parent_fiber = nullptr;

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
  inline void push_temp_obj_root(Obj **o) { temp_obj_roots.push_back(o); }
  inline void pop_temp_obj_root() { if (!temp_obj_roots.empty()) temp_obj_roots.pop_back(); }

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

  inline std::ostream &out_stream() const {
    return *current_out_port.as_ptr<ObjPort>()->out;
  }
  inline std::istream &in_stream() const {
    return *current_in_port.as_ptr<ObjPort>()->in;
  }

  // Hook for error diagnostics
  std::string last_error;

  VM() : current_fiber(nullptr) {
    heap.set_vm(this);
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
      StepResult res = run_dispatch(f, 100000000, base_depth);
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

  // Fiber Execution (Quantum Stepping)
  enum class StepResult {
    Completed,
    Yielded,
    Error
  };

  StepResult step_fiber(Fiber &f, size_t max_instructions = 1000);
  void step_all_active_fibers(size_t instructions_per_fiber = 500);

  // Shared dispatch loop body. step_fiber calls this with stop_at_depth=0
  // (today's behavior: run until the fiber is genuinely done). call_closure
  // calls it directly with stop_at_depth set to the frame depth it started
  // at, so control returns to the C++ caller once the nested call's own
  // frame(s) unwind back past that point — without touching current_fiber/
  // parent_fiber bookkeeping, since it's already correctly set up by
  // whichever step_fiber invocation is further up the (real) call chain.
  StepResult run_dispatch(Fiber &f, size_t max_instructions, size_t stop_at_depth);

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
