#pragma once

#include "vx_value.h"
#include "vx_heap.h"
#include <vector>
#include <unordered_map>
#include <string>
#include <iostream>
#include <sstream>
#include <cmath>

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
  std::vector<Value> stack;
  std::vector<CallFrame> frames;
  Value result;
  std::string error_message;

  // Continuation snapshot for call/cc
  std::vector<Value> saved_continuation;

  inline Fiber()
      : state(State::Ready), result(Value::unspecified()) {
    stack.reserve(256);
    frames.reserve(32);
  }

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

  // Hook for error diagnostics
  std::string last_error;

  VM() : current_fiber(nullptr) {
    init_primitives();
  }

  ~VM() {
    for (Fiber *f : active_fibers) delete f;
    active_fibers.clear();
  }

  Value call_closure(ObjClosure *closure, const std::vector<Value> &args) {
    Fiber f;
    f.push(Value::from_ptr(closure));
    if (closure->is_variadic) {
      if (args.size() < closure->arity) {
        last_error = "Variadic closure: expected at least " + std::to_string(closure->arity) + " args";
        return Value::unspecified();
      }
      for (size_t i = 0; i < closure->arity; ++i) f.push(args[i]);
      Value rest_list = Value::nil();
      for (size_t i = args.size(); i > closure->arity; --i) {
        rest_list = heap.cons(args[i - 1], rest_list);
      }
      f.push(rest_list);
    } else {
      for (Value a : args) f.push(a);
    }
    size_t actual_argc = closure->is_variadic ? (closure->arity + 1) : args.size();
    size_t frame_slots = std::max<size_t>(actual_argc + 1, closure->max_locals);
    f.stack.resize(frame_slots, Value::unspecified());
    f.frames.push_back({closure, closure->chunk->code.data(), 0});
    StepResult res = step_fiber(f, 10000000);
    if (res == StepResult::Error || f.state == Fiber::State::Error) {
      last_error = f.error_message;
      return Value::unspecified();
    }
    return f.result;
  }

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
