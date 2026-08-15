#include "vx_vm.h"
#include <iomanip>

namespace vxs {

// Helper to read 16-bit uint from bytecode
static inline uint16_t read_u16(const uint8_t *&ip) {
  uint16_t val = (static_cast<uint16_t>(ip[0]) << 8) | static_cast<uint16_t>(ip[1]);
  ip += 2;
  return val;
}

// Helper to read 32-bit uint from bytecode
[[maybe_unused]] static inline uint32_t read_u32(const uint8_t *&ip) {
  uint32_t val = (static_cast<uint32_t>(ip[0]) << 24) |
                 (static_cast<uint32_t>(ip[1]) << 16) |
                 (static_cast<uint32_t>(ip[2]) << 8) |
                 static_cast<uint32_t>(ip[3]);
  ip += 4;
  return val;
}

// Format Value as S-expression string
std::string VM::format_value(Value v) const {
  if (v.is_int()) {
    return std::to_string(v.as_int());
  }
  if (v.is_double()) {
    double d = v.as_double();
    std::ostringstream ss;
    ss << std::setprecision(15) << d;
    std::string s = ss.str();
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos && !std::isnan(d) && !std::isinf(d)) {
      s += ".0";
    }
    return s;
  }
  if (v.is_bool()) {
    return v.as_bool() ? "#t" : "#f";
  }
  if (v.is_nil()) {
    return "()";
  }
  if (v.is_unspecified()) {
    return "#<unspecified>";
  }
  if (v.is_eof()) {
    return "#<eof>";
  }
  if (v.is_symbol()) {
    return get_symbol_name(v.as_symbol_id());
  }
  if (v.is_ptr()) {
    Obj *obj = v.as_ptr<Obj>();
    switch (obj->type) {
      case ObjType::Cons: {
        std::string s = "(";
        Value cur = v;
        bool first = true;
        while (Heap::is_cons(cur)) {
          if (!first) s += " ";
          first = false;
          s += format_value(Heap::car(cur));
          cur = Heap::cdr(cur);
        }
        if (!cur.is_nil()) {
          s += " . " + format_value(cur);
        }
        s += ")";
        return s;
      }
      case ObjType::Vector: {
        ObjVector *ov = obj->as<ObjVector>();
        std::string s = "#(";
        for (uint32_t i = 0; i < ov->size; ++i) {
          if (i > 0) s += " ";
          s += format_value(ov->get(i));
        }
        s += ")";
        return s;
      }
      case ObjType::String: {
        ObjString *os = obj->as<ObjString>();
        return "\"" + std::string(os->view()) + "\"";
      }
      case ObjType::Symbol: {
        ObjSymbol *os = obj->as<ObjSymbol>();
        return os->name;
      }
      case ObjType::Closure: {
        return "#<procedure>";
      }
      case ObjType::Subr: {
        ObjSubr *subr = obj->as<ObjSubr>();
        return "#<primitive:" + std::string(subr->name) + ">";
      }
      case ObjType::Future: {
        ObjFuture *fut = obj->as<ObjFuture>();
        if (fut->is_completed) {
          return "#<future (completed: " + format_value(fut->result) + ")>";
        } else {
          return "#<future (pending)>";
        }
      }
      case ObjType::Fiber: {
        return "#<fiber>";
      }
    }
  }
  return "#<unknown>";
}

void VM::display_value(Value v, std::ostream &os) const {
  if (Heap::is_string(v)) {
    os << v.as_ptr<ObjString>()->view();
  } else {
    os << format_value(v);
  }
}

// Step one fiber for up to max_instructions
VM::StepResult VM::step_fiber(Fiber &f, size_t max_instructions) {
  if (f.state == Fiber::State::Completed || f.state == Fiber::State::Error) {
    return StepResult::Completed;
  }

  current_fiber = &f;
  f.state = Fiber::State::Running;

  if (f.frames.empty()) {
    f.state = Fiber::State::Completed;
    return StepResult::Completed;
  }

  CallFrame *frame = &f.frames.back();
  const uint8_t *ip = frame->ip;
  const BytecodeChunk *chunk = frame->closure->chunk;

  size_t count = 0;
  while (count < max_instructions) {
    ++count;
    uint8_t op = *ip++;

    switch (op) {
      case OP_CONST: {
        uint16_t const_ix = read_u16(ip);
        assert(const_ix < chunk->constants.size());
        f.push(chunk->constants[const_ix]);
        break;
      }
      case OP_NIL:
        f.push(Value::nil());
        break;
      case OP_TRUE:
        f.push(Value::boolean_true());
        break;
      case OP_FALSE:
        f.push(Value::boolean_false());
        break;
      case OP_UNSPECIFIED:
        f.push(Value::unspecified());
        break;

      case OP_POP:
        f.pop();
        break;

      case OP_DUP:
        f.push(f.top());
        break;

      case OP_DEF_GLOBAL: {
        uint16_t name_ix = read_u16(ip);
        Value val = f.pop();
        Value name_val = chunk->constants[name_ix];
        std::string name = get_symbol_name(name_val.as_symbol_id());
        def_global(name, val);
        break;
      }

      case OP_GET_GLOBAL: {
        uint16_t name_ix = read_u16(ip);
        Value name_val = chunk->constants[name_ix];
        std::string name = get_symbol_name(name_val.as_symbol_id());
        auto it = globals.find(name);
        if (it == globals.end()) {
          f.state = Fiber::State::Error;
          f.error_message = "[VM Error] Unbound global variable: " + name;
          return StepResult::Error;
        }
        f.push(it->second);
        break;
      }

      case OP_SET_GLOBAL: {
        uint16_t name_ix = read_u16(ip);
        Value val = f.top();
        Value name_val = chunk->constants[name_ix];
        std::string name = get_symbol_name(name_val.as_symbol_id());
        globals[name] = val;
        break;
      }

      case OP_GET_LOCAL: {
        uint16_t slot = read_u16(ip);
        f.push(f.stack[frame->stack_base + slot]);
        break;
      }

      case OP_SET_LOCAL: {
        uint16_t slot = read_u16(ip);
        f.stack[frame->stack_base + slot] = f.top();
        break;
      }

      case OP_GET_UPVALUE: {
        uint16_t slot = read_u16(ip);
        assert(slot < frame->closure->env_size);
        f.push(frame->closure->env[slot]);
        break;
      }

      case OP_SET_UPVALUE: {
        uint16_t slot = read_u16(ip);
        assert(slot < frame->closure->env_size);
        frame->closure->env[slot] = f.top();
        break;
      }

      case OP_JUMP: {
        uint16_t offset = read_u16(ip);
        ip += offset;
        break;
      }

      case OP_JUMP_IF_FALSE: {
        uint16_t offset = read_u16(ip);
        Value cond = f.pop();
        if (cond.is_false()) {
          ip += offset;
        }
        break;
      }

      case OP_JUMP_IF_TRUE: {
        uint16_t offset = read_u16(ip);
        Value cond = f.pop();
        if (cond.is_true()) {
          ip += offset;
        }
        break;
      }

      case OP_ADD: {
        Value b = f.pop();
        Value a = f.pop();
        if (a.is_int() && b.is_int()) {
          f.push(Value::from_int(a.as_int() + b.as_int()));
        } else {
          f.push(Value::from_double(a.as_real() + b.as_real()));
        }
        break;
      }

      case OP_SUB: {
        Value b = f.pop();
        Value a = f.pop();
        if (a.is_int() && b.is_int()) {
          f.push(Value::from_int(a.as_int() - b.as_int()));
        } else {
          f.push(Value::from_double(a.as_real() - b.as_real()));
        }
        break;
      }

      case OP_MUL: {
        Value b = f.pop();
        Value a = f.pop();
        if (a.is_int() && b.is_int()) {
          f.push(Value::from_int(a.as_int() * b.as_int()));
        } else {
          f.push(Value::from_double(a.as_real() * b.as_real()));
        }
        break;
      }

      case OP_DIV: {
        Value b = f.pop();
        Value a = f.pop();
        f.push(Value::from_double(a.as_real() / b.as_real()));
        break;
      }

      case OP_REM: {
        Value b = f.pop();
        Value a = f.pop();
        int64_t ia = a.is_int() ? a.as_int() : static_cast<int64_t>(a.as_real());
        int64_t ib = b.is_int() ? b.as_int() : static_cast<int64_t>(b.as_real());
        if (ib == 0) f.push(Value::from_int(0));
        else f.push(Value::from_int(static_cast<int32_t>(ia % ib)));
        break;
      }

      case OP_EQ: {
        Value b = f.pop();
        Value a = f.pop();
        f.push(Value::from_bool(a == b));
        break;
      }

      case OP_NUM_EQ: {
        Value b = f.pop();
        Value a = f.pop();
        f.push(Value::from_bool(a.as_real() == b.as_real()));
        break;
      }

      case OP_LT: {
        Value b = f.pop();
        Value a = f.pop();
        if (a.is_int() && b.is_int()) {
          f.push(Value::from_bool(a.as_int() < b.as_int()));
        } else {
          f.push(Value::from_bool(a.as_real() < b.as_real()));
        }
        break;
      }

      case OP_LE: {
        Value b = f.pop();
        Value a = f.pop();
        if (a.is_int() && b.is_int()) {
          f.push(Value::from_bool(a.as_int() <= b.as_int()));
        } else {
          f.push(Value::from_bool(a.as_real() <= b.as_real()));
        }
        break;
      }

      case OP_GT: {
        Value b = f.pop();
        Value a = f.pop();
        if (a.is_int() && b.is_int()) {
          f.push(Value::from_bool(a.as_int() > b.as_int()));
        } else {
          f.push(Value::from_bool(a.as_real() > b.as_real()));
        }
        break;
      }

      case OP_GE: {
        Value b = f.pop();
        Value a = f.pop();
        if (a.is_int() && b.is_int()) {
          f.push(Value::from_bool(a.as_int() >= b.as_int()));
        } else {
          f.push(Value::from_bool(a.as_real() >= b.as_real()));
        }
        break;
      }

      case OP_NOT: {
        Value v = f.pop();
        f.push(Value::from_bool(v.is_false()));
        break;
      }

      case OP_CALL: {
        uint8_t argc = *ip++;
        Value callee = f.top(argc);

        if (Heap::is_subr(callee)) {
          ObjSubr *subr = callee.as_ptr<ObjSubr>();
          if (argc < subr->min_args || argc > subr->max_args) {
            f.state = Fiber::State::Error;
            f.error_message = "[VM Error] " + std::string(subr->name) + ": wrong number of arguments";
            return StepResult::Error;
          }
          Value *args = &f.stack[f.stack.size() - argc];
          Value res = subr->fn(*this, argc, args);
          if (f.state == Fiber::State::Error) return StepResult::Error;
          f.stack.resize(f.stack.size() - argc - 1);
          f.push(res);
        } else if (Heap::is_closure(callee)) {
          ObjClosure *closure = callee.as_ptr<ObjClosure>();
          if (argc != closure->arity) {
            f.state = Fiber::State::Error;
            f.error_message = "[VM Error] Closure call: expected " + std::to_string(closure->arity) + " args, got " + std::to_string(argc);
            return StepResult::Error;
          }
          frame->ip = ip; // Save current IP
          size_t stack_base = f.stack.size() - argc - 1;
          size_t frame_slots = std::max<size_t>(argc + 1, closure->max_locals);
          f.stack.resize(stack_base + frame_slots, Value::unspecified());
          f.frames.push_back({closure, closure->chunk->code.data(), stack_base});
          frame = &f.frames.back();
          ip = frame->ip;
          chunk = frame->closure->chunk;
        } else {
          f.state = Fiber::State::Error;
          f.error_message = "[VM Error] Attempted to call non-procedure: " + format_value(callee);
          return StepResult::Error;
        }
        break;
      }

      case OP_TAIL_CALL: {
        uint8_t argc = *ip++;
        Value callee = f.top(argc);

        if (Heap::is_subr(callee)) {
          ObjSubr *subr = callee.as_ptr<ObjSubr>();
          Value *args = &f.stack[f.stack.size() - argc];
          Value res = subr->fn(*this, argc, args);
          if (f.state == Fiber::State::Error) return StepResult::Error;
          f.stack.resize(f.stack.size() - argc - 1);
          f.push(res);
        } else if (Heap::is_closure(callee)) {
          ObjClosure *closure = callee.as_ptr<ObjClosure>();
          if (argc != closure->arity) {
            f.state = Fiber::State::Error;
            f.error_message = "[VM Error] Closure call: expected " + std::to_string(closure->arity) + " args, got " + std::to_string(argc);
            return StepResult::Error;
          }
          // Shift callee + arguments down to current frame's stack base
          size_t old_base = frame->stack_base;
          f.stack[old_base] = callee;
          for (size_t i = 0; i < argc; ++i) {
            f.stack[old_base + 1 + i] = f.stack[f.stack.size() - argc + i];
          }
          size_t frame_slots = std::max<size_t>(argc + 1, closure->max_locals);
          f.stack.resize(old_base + frame_slots, Value::unspecified());
          frame->closure = closure;
          frame->ip = closure->chunk->code.data();
          ip = frame->ip;
          chunk = frame->closure->chunk;
        }
        break;
      }

      case OP_RETURN: {
        Value res = f.pop();
        f.stack.resize(frame->stack_base);
        f.frames.pop_back();

        if (f.frames.empty()) {
          f.result = res;
          f.state = Fiber::State::Completed;
          return StepResult::Completed;
        }

        // Push result into caller's stack slot
        f.push(res);

        frame = &f.frames.back();
        ip = frame->ip;
        chunk = frame->closure->chunk;
        break;
      }

      case OP_CLOSURE: {
        uint16_t const_ix = read_u16(ip);
        uint8_t uv_count = *ip++;
        ObjClosure *proto = chunk->constants[const_ix].as_ptr<ObjClosure>();
        ObjClosure *instance = heap.allocate<ObjClosure>(proto->chunk, proto->arity, proto->is_variadic, uv_count, proto->max_locals);
        for (uint8_t i = 0; i < uv_count; ++i) {
          uint8_t is_local = *ip++;
          uint8_t index = *ip++;
          if (is_local) {
            instance->env[i] = f.stack[frame->stack_base + index];
          } else {
            instance->env[i] = frame->closure->env[index];
          }
        }
        f.push(Value::from_ptr(instance));
        break;
      }

      case OP_YIELD: {
        frame->ip = ip;
        f.state = Fiber::State::Suspended;
        return StepResult::Yielded;
      }

      case OP_FUTURE: {
        // Pop nullary lambda closure and spawn fiber
        Value closure_val = f.pop();
        assert(Heap::is_closure(closure_val));
        ObjClosure *closure = closure_val.as_ptr<ObjClosure>();

        Fiber *child = new Fiber();
        child->push(closure_val);
        child->stack.resize(std::max<size_t>(1, closure->max_locals), Value::unspecified());
        child->frames.push_back({closure, closure->chunk->code.data(), 0});
        active_fibers.push_back(child);

        Value fut_val = heap.make_future(child);
        f.push(fut_val);
        break;
      }

      case OP_TOUCH: {
        Value fut_val = f.pop();
        if (!Heap::is_future(fut_val)) {
          f.state = Fiber::State::Error;
          f.error_message = "[VM Error] touch: expected a future";
          return StepResult::Error;
        }
        ObjFuture *fut = fut_val.as_ptr<ObjFuture>();
        if (fut->is_completed) {
          f.push(fut->result);
        } else {
          // If not completed, run child until completion
          while (fut->fiber && fut->fiber->state != Fiber::State::Completed && fut->fiber->state != Fiber::State::Error) {
            step_fiber(*fut->fiber, 1000);
          }
          fut->is_completed = true;
          fut->result = fut->fiber->result;
          f.push(fut->result);
          // Clean up finished fiber from active list
          for (auto it = active_fibers.begin(); it != active_fibers.end(); ++it) {
            if (*it == fut->fiber) {
              active_fibers.erase(it);
              delete fut->fiber;
              fut->fiber = nullptr;
              break;
            }
          }
        }
        break;
      }

      default:
        f.state = Fiber::State::Error;
        f.error_message = "[VM Error] Unknown opcode: " + std::to_string(op);
        return StepResult::Error;
    }
  }

  frame->ip = ip;
  return StepResult::Yielded;
}

// Step all active background fibers
void VM::step_all_active_fibers(size_t instructions_per_fiber) {
  for (size_t i = 0; i < active_fibers.size(); ) {
    Fiber *f = active_fibers[i];
    StepResult res = step_fiber(*f, instructions_per_fiber);
    if (res == StepResult::Completed || res == StepResult::Error) {
      active_fibers.erase(active_fibers.begin() + i);
      delete f;
    } else {
      ++i;
    }
  }
}

// Builtin primitive registration
void VM::init_primitives() {
  // 1. Math & Arithmetic
  auto subr_add = [](VM &, uint32_t argc, Value *args) -> Value {
    if (argc == 0) return Value::from_int(0);
    bool all_int = true;
    int32_t isum = 0;
    double dsum = 0.0;
    for (uint32_t i = 0; i < argc; ++i) {
      if (!args[i].is_int()) all_int = false;
      if (all_int) isum += args[i].as_int();
      dsum += args[i].as_real();
    }
    return all_int ? Value::from_int(isum) : Value::from_double(dsum);
  };
  def_global("+", heap.make_subr("+", subr_add, 0, UINT32_MAX));

  auto subr_sub = [](VM &, uint32_t argc, Value *args) -> Value {
    if (argc == 1) {
      return args[0].is_int() ? Value::from_int(-args[0].as_int()) : Value::from_double(-args[0].as_real());
    }
    bool all_int = true;
    for (uint32_t i = 0; i < argc; ++i) {
      if (!args[i].is_int()) { all_int = false; break; }
    }
    if (all_int) {
      int32_t isum = args[0].as_int();
      for (uint32_t i = 1; i < argc; ++i) isum -= args[i].as_int();
      return Value::from_int(isum);
    } else {
      double dsum = args[0].as_real();
      for (uint32_t i = 1; i < argc; ++i) dsum -= args[i].as_real();
      return Value::from_double(dsum);
    }
  };
  def_global("-", heap.make_subr("-", subr_sub, 1, UINT32_MAX));

  auto subr_mul = [](VM &, uint32_t argc, Value *args) -> Value {
    if (argc == 0) return Value::from_int(1);
    bool all_int = true;
    int32_t iprod = 1;
    double dprod = 1.0;
    for (uint32_t i = 0; i < argc; ++i) {
      if (!args[i].is_int()) all_int = false;
      if (all_int) iprod *= args[i].as_int();
      dprod *= args[i].as_real();
    }
    return all_int ? Value::from_int(iprod) : Value::from_double(dprod);
  };
  def_global("*", heap.make_subr("*", subr_mul, 0, UINT32_MAX));

  auto subr_div = [](VM &, uint32_t argc, Value *args) -> Value {
    if (argc == 1) return Value::from_double(1.0 / args[0].as_real());
    double quot = args[0].as_real();
    for (uint32_t i = 1; i < argc; ++i) quot /= args[i].as_real();
    return Value::from_double(quot);
  };
  def_global("/", heap.make_subr("/", subr_div, 1, UINT32_MAX));

  auto subr_rem = [](VM &, uint32_t, Value *args) -> Value {
    int64_t a = args[0].is_int() ? args[0].as_int() : static_cast<int64_t>(args[0].as_real());
    int64_t b = args[1].is_int() ? args[1].as_int() : static_cast<int64_t>(args[1].as_real());
    if (b == 0) return Value::from_int(0);
    return Value::from_int(static_cast<int32_t>(a % b));
  };
  def_global("remainder", heap.make_subr("remainder", subr_rem, 2, 2));
  def_global("modulo", heap.make_subr("modulo", subr_rem, 2, 2));

  // Comparisons
  auto subr_num_eq = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].as_real() == args[1].as_real());
  };
  def_global("=", heap.make_subr("=", subr_num_eq, 2, 2));

  auto subr_lt = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].as_real() < args[1].as_real());
  };
  def_global("<", heap.make_subr("<", subr_lt, 2, 2));

  auto subr_le = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].as_real() <= args[1].as_real());
  };
  def_global("<=", heap.make_subr("<=", subr_le, 2, 2));

  auto subr_gt = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].as_real() > args[1].as_real());
  };
  def_global(">", heap.make_subr(">", subr_gt, 2, 2));

  auto subr_ge = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].as_real() >= args[1].as_real());
  };
  def_global(">=", heap.make_subr(">=", subr_ge, 2, 2));

  auto subr_not = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_false());
  };
  def_global("not", heap.make_subr("not", subr_not, 1, 1));

  auto subr_eq = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0] == args[1]);
  };
  def_global("eq?", heap.make_subr("eq?", subr_eq, 2, 2));
  def_global("eqv?", heap.make_subr("eqv?", subr_eq, 2, 2));

  // Math Functions
  auto subr_sin = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_double(std::sin(args[0].as_real()));
  };
  def_global("sin", heap.make_subr("sin", subr_sin, 1, 1));

  auto subr_cos = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_double(std::cos(args[0].as_real()));
  };
  def_global("cos", heap.make_subr("cos", subr_cos, 1, 1));

  auto subr_exp = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_double(std::exp(args[0].as_real()));
  };
  def_global("exp", heap.make_subr("exp", subr_exp, 1, 1));

  auto subr_floor = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_int(static_cast<int32_t>(std::floor(args[0].as_real())));
  };
  def_global("floor", heap.make_subr("floor", subr_floor, 1, 1));

  auto subr_ceiling = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_int(static_cast<int32_t>(std::ceil(args[0].as_real())));
  };
  def_global("ceiling", heap.make_subr("ceiling", subr_ceiling, 1, 1));

  auto subr_round = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_int(static_cast<int32_t>(std::round(args[0].as_real())));
  };
  def_global("round", heap.make_subr("round", subr_round, 1, 1));

  auto subr_inexact_exact = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_int(static_cast<int32_t>(args[0].as_real()));
  };
  def_global("inexact->exact", heap.make_subr("inexact->exact", subr_inexact_exact, 1, 1));
  def_global("exact->inexact", heap.make_subr("exact->inexact", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_double(args[0].as_real());
  }, 1, 1));

  auto subr_abs = [](VM &, uint32_t, Value *args) -> Value {
    if (args[0].is_int()) return Value::from_int(std::abs(args[0].as_int()));
    return Value::from_double(std::abs(args[0].as_real()));
  };
  def_global("abs", heap.make_subr("abs", subr_abs, 1, 1));

  auto subr_sqrt = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_double(std::sqrt(args[0].as_real()));
  };
  def_global("sqrt", heap.make_subr("sqrt", subr_sqrt, 1, 1));

  auto subr_random = [](VM &, uint32_t argc, Value *args) -> Value {
    if (argc == 0) {
      return Value::from_double(static_cast<double>(std::rand()) / static_cast<double>(RAND_MAX));
    }
    if (args[0].is_int()) {
      int32_t n = args[0].as_int();
      return Value::from_int(n > 0 ? (std::rand() % n) : 0);
    }
    return Value::from_double((static_cast<double>(std::rand()) / static_cast<double>(RAND_MAX)) * args[0].as_real());
  };
  def_global("random", heap.make_subr("random", subr_random, 0, 1));

  // Cons / List
  auto subr_cons = [](VM &vm, uint32_t, Value *args) -> Value {
    return vm.heap.cons(args[0], args[1]);
  };
  def_global("cons", heap.make_subr("cons", subr_cons, 2, 2));

  auto subr_car = [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_cons(args[0])) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] car: contract violation, expected pair, got " + vm.format_value(args[0]);
      }
      return Value::unspecified();
    }
    return Heap::car(args[0]);
  };
  def_global("car", heap.make_subr("car", subr_car, 1, 1));

  auto subr_cdr = [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_cons(args[0])) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] cdr: contract violation, expected pair, got " + vm.format_value(args[0]);
      }
      return Value::unspecified();
    }
    return Heap::cdr(args[0]);
  };
  def_global("cdr", heap.make_subr("cdr", subr_cdr, 1, 1));

  auto subr_set_car = [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_cons(args[0])) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] set-car!: contract violation, expected pair, got " + vm.format_value(args[0]);
      }
      return Value::unspecified();
    }
    Heap::set_car(args[0], args[1]);
    return Value::unspecified();
  };
  def_global("set-car!", heap.make_subr("set-car!", subr_set_car, 2, 2));

  auto subr_set_cdr = [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_cons(args[0])) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] set-cdr!: contract violation, expected pair, got " + vm.format_value(args[0]);
      }
      return Value::unspecified();
    }
    Heap::set_cdr(args[0], args[1]);
    return Value::unspecified();
  };
  def_global("set-cdr!", heap.make_subr("set-cdr!", subr_set_cdr, 2, 2));

  auto subr_cadr = [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_cons(args[0]) || !Heap::is_cons(Heap::cdr(args[0]))) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] cadr: contract violation, expected list with at least 2 elements, got " + vm.format_value(args[0]);
      }
      return Value::unspecified();
    }
    return Heap::car(Heap::cdr(args[0]));
  };
  def_global("cadr", heap.make_subr("cadr", subr_cadr, 1, 1));

  auto subr_caddr = [](VM &vm, uint32_t, Value *args) -> Value {
    Value d1 = Heap::is_cons(args[0]) ? Heap::cdr(args[0]) : Value::nil();
    Value d2 = Heap::is_cons(d1) ? Heap::cdr(d1) : Value::nil();
    if (!Heap::is_cons(d2)) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] caddr: contract violation, expected list with at least 3 elements, got " + vm.format_value(args[0]);
      }
      return Value::unspecified();
    }
    return Heap::car(d2);
  };
  def_global("caddr", heap.make_subr("caddr", subr_caddr, 1, 1));

  auto subr_cdar = [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_cons(args[0]) || !Heap::is_cons(Heap::car(args[0]))) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] cdar: contract violation, expected pair of pair, got " + vm.format_value(args[0]);
      }
      return Value::unspecified();
    }
    return Heap::cdr(Heap::car(args[0]));
  };
  def_global("cdar", heap.make_subr("cdar", subr_cdar, 1, 1));

  auto subr_caar = [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_cons(args[0]) || !Heap::is_cons(Heap::car(args[0]))) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] caar: contract violation, expected pair of pair, got " + vm.format_value(args[0]);
      }
      return Value::unspecified();
    }
    return Heap::car(Heap::car(args[0]));
  };
  def_global("caar", heap.make_subr("caar", subr_caar, 1, 1));

  auto subr_list = [](VM &vm, uint32_t argc, Value *args) -> Value {
    Value res = Value::nil();
    for (int i = static_cast<int>(argc) - 1; i >= 0; --i) {
      res = vm.heap.cons(args[i], res);
    }
    return res;
  };
  def_global("list", heap.make_subr("list", subr_list, 0, UINT32_MAX));

  auto subr_length = [](VM &, uint32_t, Value *args) -> Value {
    int32_t len = 0;
    Value cur = args[0];
    while (Heap::is_cons(cur)) {
      ++len;
      cur = Heap::cdr(cur);
    }
    return Value::from_int(len);
  };
  def_global("length", heap.make_subr("length", subr_length, 1, 1));

  auto subr_reverse = [](VM &vm, uint32_t, Value *args) -> Value {
    Value cur = args[0];
    Value res = Value::nil();
    while (Heap::is_cons(cur)) {
      res = vm.heap.cons(Heap::car(cur), res);
      cur = Heap::cdr(cur);
    }
    return res;
  };
  def_global("reverse", heap.make_subr("reverse", subr_reverse, 1, 1));

  auto subr_append = [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (argc == 0) return Value::nil();
    if (argc == 1) return args[0];
    std::vector<Value> all_items;
    for (uint32_t i = 0; i < argc - 1; ++i) {
      Value cur = args[i];
      while (Heap::is_cons(cur)) {
        all_items.push_back(Heap::car(cur));
        cur = Heap::cdr(cur);
      }
    }
    Value res = args[argc - 1];
    for (auto it = all_items.rbegin(); it != all_items.rend(); ++it) {
      res = vm.heap.cons(*it, res);
    }
    return res;
  };
  def_global("append", heap.make_subr("append", subr_append, 0, UINT32_MAX));

  // High-order procedures
  auto subr_apply = [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (argc < 2) return Value::unspecified();
    Value fn = args[0];
    std::vector<Value> flat_args;
    for (uint32_t i = 1; i < argc - 1; ++i) {
      flat_args.push_back(args[i]);
    }
    Value lst = args[argc - 1];
    while (Heap::is_cons(lst)) {
      flat_args.push_back(Heap::car(lst));
      lst = Heap::cdr(lst);
    }
    if (Heap::is_subr(fn)) {
      ObjSubr *subr = fn.as_ptr<ObjSubr>();
      return subr->fn(vm, static_cast<uint32_t>(flat_args.size()), flat_args.data());
    }
    if (Heap::is_closure(fn)) {
      ObjClosure *closure = fn.as_ptr<ObjClosure>();
      Fiber child;
      child.push(fn);
      for (Value a : flat_args) child.push(a);
      child.frames.push_back({closure, closure->chunk->code.data(), 0});
      vm.step_fiber(child, 1000000);
      return child.result;
    }
    return Value::unspecified();
  };
  def_global("apply", heap.make_subr("apply", subr_apply, 2, UINT32_MAX));

  auto subr_map = [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (argc != 2) return Value::nil();
    Value fn = args[0];
    Value cur = args[1];
    std::vector<Value> results;
    while (Heap::is_cons(cur)) {
      Value elem = Heap::car(cur);
      Value out = Value::nil();
      if (Heap::is_subr(fn)) {
        out = fn.as_ptr<ObjSubr>()->fn(vm, 1, &elem);
      } else if (Heap::is_closure(fn)) {
        ObjClosure *cl = fn.as_ptr<ObjClosure>();
        Fiber child;
        child.push(fn);
        child.push(elem);
        child.frames.push_back({cl, cl->chunk->code.data(), 0});
        vm.step_fiber(child, 1000000);
        out = child.result;
      }
      results.push_back(out);
      cur = Heap::cdr(cur);
    }
    Value res = Value::nil();
    for (auto it = results.rbegin(); it != results.rend(); ++it) {
      res = vm.heap.cons(*it, res);
    }
    return res;
  };
  def_global("map", heap.make_subr("map", subr_map, 2, 2));

  auto subr_for_each = [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (argc != 2) return Value::unspecified();
    Value fn = args[0];
    Value cur = args[1];
    while (Heap::is_cons(cur)) {
      Value elem = Heap::car(cur);
      if (Heap::is_subr(fn)) {
        fn.as_ptr<ObjSubr>()->fn(vm, 1, &elem);
      } else if (Heap::is_closure(fn)) {
        ObjClosure *cl = fn.as_ptr<ObjClosure>();
        Fiber child;
        child.push(fn);
        child.push(elem);
        child.frames.push_back({cl, cl->chunk->code.data(), 0});
        vm.step_fiber(child, 1000000);
      }
      cur = Heap::cdr(cur);
    }
    return Value::unspecified();
  };
  def_global("for-each", heap.make_subr("for-each", subr_for_each, 2, 2));

  auto subr_null_p = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_nil());
  };
  def_global("null?", heap.make_subr("null?", subr_null_p, 1, 1));

  auto subr_pair_p = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_cons(args[0]));
  };
  def_global("pair?", heap.make_subr("pair?", subr_pair_p, 1, 1));

  // Display / Output
  auto subr_display = [](VM &vm, uint32_t, Value *args) -> Value {
    vm.display_value(args[0], std::cout);
    return Value::unspecified();
  };
  def_global("display", heap.make_subr("display", subr_display, 1, 1));

  auto subr_newline = [](VM &, uint32_t, Value *) -> Value {
    std::cout << std::endl;
    return Value::unspecified();
  };
  def_global("newline", heap.make_subr("newline", subr_newline, 0, 0));

  // Concurrency
  auto subr_active_count = [](VM &vm, uint32_t, Value *) -> Value {
    return Value::from_int(static_cast<int32_t>(vm.active_fibers.size()));
  };
  def_global("active-fibers-count", heap.make_subr("active-fibers-count", subr_active_count, 0, 0));

  auto subr_run_fibers = [](VM &vm, uint32_t argc, Value *args) -> Value {
    size_t max_steps = argc > 0 ? static_cast<size_t>(args[0].as_real()) : 100000;
    size_t total = 0;
    while (!vm.active_fibers.empty() && total < max_steps) {
      vm.step_all_active_fibers(100);
      total += 100;
    }
    return Value::from_int(static_cast<int32_t>(vm.active_fibers.size()));
  };
  def_global("run-fibers", heap.make_subr("run-fibers", subr_run_fibers, 0, 1));
}

} // namespace vxs
