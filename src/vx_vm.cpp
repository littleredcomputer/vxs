#include "vx_vm.h"
#include "vx_reader.h"
#include "vx_compiler.h"
#include <algorithm>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <chrono>
#include <cerrno>
#include <charconv>

namespace vxs {

// Helper to read 16-bit uint from bytecode
static inline uint16_t read_u16(const uint8_t *&ip) {
  uint16_t val = (static_cast<uint16_t>(ip[0]) << 8) | static_cast<uint16_t>(ip[1]);
  ip += 2;
  return val;
}

// Shortest decimal string that reads back to exactly `d`, per std::to_chars'
// floating-point contract (C++17, <charconv>) — the same "correctly
// rounded, minimal digits" problem Steele & White's Dragon4 solved,
// implemented in the standard library rather than by hand here. Strictly
// better than a fixed setprecision: no trailing noise digits, and no risk
// of printing too few digits to round-trip.
static std::string format_double(double d) {
  char buf[32];
  auto res = std::to_chars(buf, buf + sizeof(buf), d);
  std::string s(buf, res.ptr);
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
      !std::isnan(d) && !std::isinf(d)) {
    s += ".0";
  }
  return s;
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
    return format_double(v.as_double());
  }
  if (v.is_char()) {
    char c = v.as_char();
    if (c == ' ') return "#\\space";
    if (c == '\n') return "#\\newline";
    if (c == '\t') return "#\\tab";
    if (c == '\r') return "#\\return";
    return std::string("#\\") + c;
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
  if (v.is_keyword()) {
    return ":" + get_symbol_name(v.as_keyword_id());
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
        std::string s = "[";
        for (uint32_t i = 0; i < ov->size; ++i) {
          if (i > 0) s += " ";
          s += format_value(ov->get(i));
        }
        s += "]";
        return s;
      }
      case ObjType::Map: {
        ObjMap *m = obj->as<ObjMap>();
        std::string s = "{";
        for (size_t i = 0; i < m->entries.size(); ++i) {
          if (i > 0) s += " ";
          s += format_value(m->entries[i].first) + " " + format_value(m->entries[i].second);
        }
        s += "}";
        return s;
      }
      case ObjType::String: {
        ObjString *os = obj->as<ObjString>();
        std::string s = "\"";
        for (char c : os->view()) {
          if (c == '"' || c == '\\') s += '\\';
          s += c;
        }
        s += "\"";
        return s;
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
      case ObjType::Upvalue: {
        ObjUpvalue *uv = obj->as<ObjUpvalue>();
        return format_value(uv->value);
      }
      case ObjType::Port: {
        ObjPort *p = obj->as<ObjPort>();
        return std::string("#<") + (p->is_input ? "input" : "output") +
               "-port" + (p->closed ? " (closed)" : "") + ">";
      }
    }
  }
  return "#<unknown>";
}

void VM::display_value(Value v, std::ostream &os) const {
  if (Heap::is_string(v)) {
    os << v.as_ptr<ObjString>()->view();
  } else if (v.is_char()) {
    os << v.as_char();
  } else {
    os << format_value(v);
  }
}

//=============================================================================
// Mark-and-Sweep Garbage Collector Implementation
//=============================================================================

void Heap::mark_fiber(Fiber *f) {
  if (!f) return;
  for (size_t i = 0; i < f->stack.size(); ++i) {
    mark_value(f->stack[i]);
  }
  for (const CallFrame &frame : f->frames) {
    if (frame.closure) {
      mark_obj(frame.closure);
    }
  }
  mark_value(f->result);
  for (Value v : f->saved_continuation) {
    mark_value(v);
  }
  // Pending unwind-protect cleanups are live closures a suspended fiber
  // will eventually call — invisible everywhere else, rooted here.
  for (Value v : f->winders) {
    mark_value(v);
  }
}

void Heap::blacken_obj(Obj *obj) {
  if (!obj) return;
  switch (obj->type) {
    case ObjType::Cons: {
      auto *c = static_cast<ObjCons *>(obj);
      mark_value(c->car);
      mark_value(c->cdr);
      break;
    }
    case ObjType::Vector: {
      auto *vec = static_cast<ObjVector *>(obj);
      for (uint32_t i = 0; i < vec->size; ++i) {
        mark_value(vec->data[i]);
      }
      break;
    }
    case ObjType::Closure: {
      auto *cl = static_cast<ObjClosure *>(obj);
      if (cl->chunk) {
        for (Value c : cl->chunk->constants) {
          mark_value(c);
        }
      }
      for (uint32_t i = 0; i < cl->env_size; ++i) {
        mark_value(cl->env[i]);
      }
      break;
    }
    case ObjType::Map: {
      auto *m = static_cast<ObjMap *>(obj);
      for (const auto &pair : m->entries) {
        mark_value(pair.first);
        mark_value(pair.second);
      }
      break;
    }
    case ObjType::Future: {
      auto *fut = static_cast<ObjFuture *>(obj);
      mark_value(fut->result);
      if (fut->fiber) {
        mark_fiber(fut->fiber);
      }
      break;
    }
    case ObjType::Upvalue: {
      auto *uv = static_cast<ObjUpvalue *>(obj);
      mark_value(uv->value);
      break;
    }
    case ObjType::String:
    case ObjType::Symbol:
    case ObjType::Subr:
    case ObjType::Fiber:
    case ObjType::Port:
      // Leaf objects - no child references
      break;
  }
}

void VM::mark_roots(Heap &h) {
  h.mark_value(stdin_port);
  h.mark_value(stdout_port);
  h.mark_value(current_in_port);
  h.mark_value(current_out_port);
  for (const auto &kv : globals) {
    h.mark_value(kv.second);
  }
  for (const auto &kv : macros) {
    if (kv.second) h.mark_obj(kv.second);
  }
  for (const auto &kv : property_table) {
    h.mark_value(kv.second);
  }
  for (Fiber *f : active_fibers) {
    h.mark_fiber(f);
  }
  for (Fiber *f = current_fiber; f != nullptr; f = f->parent_fiber) {
    h.mark_fiber(f);
  }
  for (Value *vp : temp_roots) {
    if (vp) h.mark_value(*vp);
  }
  for (Obj **op : temp_obj_roots) {
    if (op && *op) h.mark_obj(*op);
  }
  for (Value v : in_flight_raises) {
    h.mark_value(v);
  }
}

void Heap::collect_garbage() {
  if (gc_paused_depth > 0 || !vm) return;

  // 1. Mark phase
  vm->mark_roots(*this);

  // Drain gray stack
  while (!gray_stack.empty()) {
    Obj *obj = gray_stack.back();
    gray_stack.pop_back();
    blacken_obj(obj);
  }

  // 2. Sweep phase
  sweep();

  // 3. Dynamic threshold adjustment (grow by 2x of live bytes, floor is
  // min_gc_threshold — normally 512KB, but overridable via
  // set_gc_threshold/--gc-threshold so a caller lowering it for GC-pressure
  // testing doesn't just get one aggressive collection before it snaps
  // back to the default).
  gc_threshold = std::max(min_gc_threshold, bytes_allocated * 2);
}

size_t Heap::sweep() {
  size_t freed_count = 0;
  Obj **cur = &head_obj;
  while (*cur) {
    Obj *obj = *cur;
    if (!obj->gc_mark) {
      *cur = obj->next_all;
      bytes_allocated -= obj_allocated_size(obj);
      destroy_obj(obj);
      ++freed_count;
    } else {
      obj->gc_mark = false; // Reset mark for next cycle
      cur = &obj->next_all;
    }
  }
  return freed_count;
}

// Step one fiber — to its own yield/completion/error by default, or to
// an opt-in instruction cap / wall-clock deadline (see StepResult's
// comment in vx_vm.h for the scheduling model these must respect).
VM::StepResult VM::step_fiber(Fiber &f, size_t max_instructions,
                              std::chrono::steady_clock::time_point deadline) {
  if (f.state == Fiber::State::Completed || f.state == Fiber::State::Error) {
    return StepResult::Completed;
  }

  f.parent_fiber = current_fiber;
  current_fiber = &f;
  f.state = Fiber::State::Running;

  struct FiberGuard {
    VM &vm;
    Fiber &fiber;
    FiberGuard(VM &v, Fiber &fb) : vm(v), fiber(fb) {}
    ~FiberGuard() {
      vm.current_fiber = fiber.parent_fiber;
      fiber.parent_fiber = nullptr;
    }
  } fiber_guard(*this, f);

  if (f.frames.empty()) {
    f.state = Fiber::State::Completed;
    return StepResult::Completed;
  }

  try {
    StepResult res = run_dispatch(f, max_instructions, 0, deadline);
    if (res == StepResult::Error) {
      // Error is terminal for a fiber; its pending unwind-protect
      // cleanups run now, before anyone inspects the corpse — this is
      // what keeps e.g. with-output-to-file's redirection from
      // outliving a crashed body.
      run_pending_winders(f, 0);
    }
    return res;
  } catch (ContinuationEscape &) {
    // An escape targeting nothing in this fiber (invoked outside its
    // dynamic extent) is leaving the VM entirely — run the cleanups on
    // its way through, then let it surface as the usual top-level error.
    run_pending_winders(f, 0);
    throw;
  }
}

void VM::run_pending_winders(Fiber &f, size_t down_to) {
  Fiber::State saved_state = f.state;
  std::string saved_error = f.error_message;
  while (f.winders.size() > down_to) {
    Value w = f.winders.back();
    f.winders.pop_back();
    if (!Heap::is_closure(w)) continue;
    // call_closure needs a runnable fiber; the original disposition
    // (and error message — first error wins) is restored afterward.
    f.state = Fiber::State::Running;
    call_closure(w.as_ptr<ObjClosure>(), {});
  }
  f.state = saved_state;
  f.error_message = saved_error;
}

// The dispatch loop proper — see the declaration in vx_vm.h for how
// stop_at_depth lets call_closure re-enter this on an already-running
// fiber without going through step_fiber's setup again, and for why the
// deadline is checked only here, never in a nested call_closure dispatch.
VM::StepResult VM::run_dispatch(Fiber &f, size_t max_instructions, size_t stop_at_depth,
                                std::chrono::steady_clock::time_point deadline) {
  CallFrame *frame = &f.frames.back();
  const uint8_t *ip = frame->ip;
  const BytecodeChunk *chunk = frame->closure->chunk.get();

  size_t count = 0;
  while (count < max_instructions) {
    ++count;
    // Wall-clock backstop, sampled every 1024 instructions (bitmask, not
    // a divide) and short-circuited away entirely when no deadline is
    // set — the common native/synchronous case pays one predicted branch.
    if (deadline != NO_DEADLINE && (count & 0x3FF) == 0 &&
        std::chrono::steady_clock::now() >= deadline) {
      frame->ip = ip;
      f.state = Fiber::State::Suspended; // suspended, just not voluntarily —
      return StepResult::Preempted;      // the result carries that bit
    }
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
        Value v = f.stack[frame->stack_base + slot];
        if (Heap::is_upvalue(v)) {
          f.push(v.as_ptr<ObjUpvalue>()->value);
        } else {
          f.push(v);
        }
        break;
      }

      case OP_SET_LOCAL: {
        uint16_t slot = read_u16(ip);
        Value &slot_ref = f.stack[frame->stack_base + slot];
        if (Heap::is_upvalue(slot_ref)) {
          slot_ref.as_ptr<ObjUpvalue>()->value = f.top();
        } else {
          slot_ref = f.top();
        }
        break;
      }

      case OP_GET_UPVALUE: {
        uint16_t slot = read_u16(ip);
        assert(slot < frame->closure->env_size);
        Value uv = frame->closure->env[slot];
        if (Heap::is_upvalue(uv)) {
          f.push(uv.as_ptr<ObjUpvalue>()->value);
        } else {
          f.push(uv);
        }
        break;
      }

      case OP_SET_UPVALUE: {
        uint16_t slot = read_u16(ip);
        assert(slot < frame->closure->env_size);
        Value uv = frame->closure->env[slot];
        if (Heap::is_upvalue(uv)) {
          uv.as_ptr<ObjUpvalue>()->value = f.top();
        } else {
          frame->closure->env[slot] = f.top();
        }
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
          int64_t sum = static_cast<int64_t>(a.as_int()) + static_cast<int64_t>(b.as_int());
          if (sum >= INT32_MIN && sum <= INT32_MAX) {
            f.push(Value::from_int(static_cast<int32_t>(sum)));
          } else {
            f.push(Value::from_double(static_cast<double>(sum)));
          }
        } else {
          f.push(Value::from_double(a.as_real() + b.as_real()));
        }
        break;
      }

      case OP_SUB: {
        Value b = f.pop();
        Value a = f.pop();
        if (a.is_int() && b.is_int()) {
          int64_t diff = static_cast<int64_t>(a.as_int()) - static_cast<int64_t>(b.as_int());
          if (diff >= INT32_MIN && diff <= INT32_MAX) {
            f.push(Value::from_int(static_cast<int32_t>(diff)));
          } else {
            f.push(Value::from_double(static_cast<double>(diff)));
          }
        } else {
          f.push(Value::from_double(a.as_real() - b.as_real()));
        }
        break;
      }

      case OP_MUL: {
        Value b = f.pop();
        Value a = f.pop();
        if (a.is_int() && b.is_int()) {
          int64_t prod = static_cast<int64_t>(a.as_int()) * static_cast<int64_t>(b.as_int());
          if (prod >= INT32_MIN && prod <= INT32_MAX) {
            f.push(Value::from_int(static_cast<int32_t>(prod)));
          } else {
            f.push(Value::from_double(static_cast<double>(prod)));
          }
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
          Value *args;
          std::vector<Value> args_scratch;
          size_t args_start = f.stack.size() - argc;
          if (!f.stack.contiguous_range(args_start, argc, &args)) {
            // Rare: this call's argument window straddles a slab
            // boundary. Copy it into a scratch buffer so subr bodies can
            // keep doing ordinary args[i] pointer arithmetic.
            args_scratch.resize(argc);
            for (uint8_t i = 0; i < argc; ++i) args_scratch[i] = f.stack[args_start + i];
            args = args_scratch.data();
          }
          Value res = call_subr(subr, argc, args);
          if (f.state == Fiber::State::Error) return StepResult::Error;
          f.stack.resize(f.stack.size() - argc - 1);
          f.push(res);
        } else if (Heap::is_closure(callee)) {
          ObjClosure *closure = callee.as_ptr<ObjClosure>();
          if (closure->is_variadic) {
            if (argc < closure->arity) {
              f.state = Fiber::State::Error;
              f.error_message = "[VM Error] Variadic closure call: expected at least " + std::to_string(closure->arity) + " args, got " + std::to_string(argc);
              return StepResult::Error;
            }
            Value rest_list = Value::nil();
            push_temp_root(&rest_list);
            for (size_t i = argc; i > closure->arity; --i) {
              Value arg_val = f.stack[f.stack.size() - argc + (i - 1)];
              rest_list = heap.cons(arg_val, rest_list);
            }
            pop_temp_root();
            size_t old_top = f.stack.size() - argc;
            f.stack.resize(old_top + closure->arity);
            f.push(rest_list);
            argc = closure->arity + 1;
          } else {
            if (argc != closure->arity) {
              f.state = Fiber::State::Error;
              f.error_message = "[VM Error] Closure call: expected " + std::to_string(closure->arity) + " args, got " + std::to_string(argc);
              return StepResult::Error;
            }
          }
          frame->ip = ip; // Save current IP
          size_t stack_base = f.stack.size() - argc - 1;
          size_t frame_slots = std::max<size_t>(argc + 1, closure->max_locals);
          f.stack.resize(stack_base + frame_slots, Value::unspecified());
          f.frames.push_back({closure, closure->chunk->code.data(), stack_base});
          frame = &f.frames.back();
          ip = frame->ip;
          chunk = frame->closure->chunk.get();
        } else if (callee.is_keyword()) {
          // Keyword as procedure: (:key map [default])
          if (argc < 1) {
            f.state = Fiber::State::Error;
            f.error_message = "[VM Error] Keyword procedure requires at least 1 argument";
            return StepResult::Error;
          }
          Value target = f.stack[f.stack.size() - argc];
          Value def_val = argc > 1 ? f.stack[f.stack.size() - argc + 1] : Value::nil();
          Value res = def_val;
          if (Heap::is_map(target)) {
            res = target.as_ptr<ObjMap>()->get(callee, def_val);
          }
          f.stack.resize(f.stack.size() - argc - 1);
          f.push(res);
        } else if (Heap::is_map(callee)) {
          // Map as procedure: (map key [default])
          if (argc < 1) {
            f.state = Fiber::State::Error;
            f.error_message = "[VM Error] Map procedure requires at least 1 argument";
            return StepResult::Error;
          }
          ObjMap *m = callee.as_ptr<ObjMap>();
          Value key = f.stack[f.stack.size() - argc];
          Value def_val = argc > 1 ? f.stack[f.stack.size() - argc + 1] : Value::nil();
          Value res = m->get(key, def_val);
          f.stack.resize(f.stack.size() - argc - 1);
          f.push(res);
        } else if (Heap::is_vector(callee)) {
          // Vector as procedure: (vector index [default])
          if (argc < 1) {
            f.state = Fiber::State::Error;
            f.error_message = "[VM Error] Vector procedure requires at least 1 argument";
            return StepResult::Error;
          }
          ObjVector *v = callee.as_ptr<ObjVector>();
          Value ix_val = f.stack[f.stack.size() - argc];
          Value def_val = argc > 1 ? f.stack[f.stack.size() - argc + 1] : Value::nil();
          Value res = def_val;
          if (ix_val.is_int()) {
            int32_t ix = ix_val.as_int();
            if (ix >= 0 && static_cast<uint32_t>(ix) < v->size) {
              res = v->get(static_cast<uint32_t>(ix));
            }
          }
          f.stack.resize(f.stack.size() - argc - 1);
          f.push(res);
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

        auto return_tail_val = [&](Value res) -> bool {
          f.stack.resize(frame->stack_base);
          f.frames.pop_back();

          if (f.frames.size() <= stop_at_depth) {
            f.result = res;
            if (stop_at_depth == 0) {
              f.state = Fiber::State::Completed;
            }
            return false;
          }

          f.push(res);
          frame = &f.frames.back();
          ip = frame->ip;
          chunk = frame->closure->chunk.get();
          return true;
        };

        if (Heap::is_subr(callee)) {
          ObjSubr *subr = callee.as_ptr<ObjSubr>();
          if (argc < subr->min_args || argc > subr->max_args) {
            f.state = Fiber::State::Error;
            f.error_message = "[VM Error] " + std::string(subr->name) + ": wrong number of arguments";
            return StepResult::Error;
          }
          Value *args;
          std::vector<Value> args_scratch;
          size_t args_start = f.stack.size() - argc;
          if (!f.stack.contiguous_range(args_start, argc, &args)) {
            args_scratch.resize(argc);
            for (uint8_t i = 0; i < argc; ++i) args_scratch[i] = f.stack[args_start + i];
            args = args_scratch.data();
          }
          Value res = call_subr(subr, argc, args);
          if (f.state == Fiber::State::Error) {
            return StepResult::Error;
          }
          if (!return_tail_val(res)) return StepResult::Completed;
        } else if (Heap::is_closure(callee)) {
          ObjClosure *closure = callee.as_ptr<ObjClosure>();
          if (closure->is_variadic) {
            if (argc < closure->arity) {
              f.state = Fiber::State::Error;
              f.error_message = "[VM Error] Variadic closure call: expected at least " + std::to_string(closure->arity) + " args, got " + std::to_string(argc);
              return StepResult::Error;
            }
            Value rest_list = Value::nil();
            push_temp_root(&rest_list);
            for (size_t i = argc; i > closure->arity; --i) {
              Value arg_val = f.stack[f.stack.size() - argc + (i - 1)];
              rest_list = heap.cons(arg_val, rest_list);
            }
            pop_temp_root();
            size_t old_top = f.stack.size() - argc;
            f.stack.resize(old_top + closure->arity);
            f.push(rest_list);
            argc = closure->arity + 1;
          } else {
            if (argc != closure->arity) {
              f.state = Fiber::State::Error;
              f.error_message = "[VM Error] Closure call: expected " + std::to_string(closure->arity) + " args, got " + std::to_string(argc);
              return StepResult::Error;
            }
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
          chunk = frame->closure->chunk.get();
        } else if (callee.is_keyword()) {
          Value target = f.stack[f.stack.size() - argc];
          Value def_val = argc > 1 ? f.stack[f.stack.size() - argc + 1] : Value::nil();
          Value res = def_val;
          if (Heap::is_map(target)) {
            res = target.as_ptr<ObjMap>()->get(callee, def_val);
          }
          if (!return_tail_val(res)) return StepResult::Completed;
        } else if (Heap::is_map(callee)) {
          ObjMap *m = callee.as_ptr<ObjMap>();
          Value key = f.stack[f.stack.size() - argc];
          Value def_val = argc > 1 ? f.stack[f.stack.size() - argc + 1] : Value::nil();
          Value res = m->get(key, def_val);
          if (!return_tail_val(res)) return StepResult::Completed;
        } else if (Heap::is_vector(callee)) {
          ObjVector *v = callee.as_ptr<ObjVector>();
          Value ix_val = f.stack[f.stack.size() - argc];
          Value def_val = argc > 1 ? f.stack[f.stack.size() - argc + 1] : Value::nil();
          Value res = def_val;
          if (ix_val.is_int()) {
            int32_t ix = ix_val.as_int();
            if (ix >= 0 && static_cast<uint32_t>(ix) < v->size) {
              res = v->get(static_cast<uint32_t>(ix));
            }
          }
          if (!return_tail_val(res)) return StepResult::Completed;
        } else {
          f.state = Fiber::State::Error;
          f.error_message = "[VM Error] Attempted to call non-procedure: " + format_value(callee);
          return StepResult::Error;
        }
        break;
      }

      case OP_RETURN: {
        Value res = f.pop();
        f.stack.resize(frame->stack_base);
        f.frames.pop_back();

        if (f.frames.size() <= stop_at_depth) {
          f.result = res;
          if (stop_at_depth == 0) {
            f.state = Fiber::State::Completed;
          }
          return StepResult::Completed;
        }

        // Push result into caller's stack slot
        f.push(res);

        frame = &f.frames.back();
        ip = frame->ip;
        chunk = frame->closure->chunk.get();
        break;
      }

      case OP_CLOSURE: {
        uint16_t const_ix = read_u16(ip);
        uint8_t uv_count = *ip++;
        ObjClosure *proto = chunk->constants[const_ix].as_ptr<ObjClosure>();
        ObjClosure *instance = heap.allocate<ObjClosure>(proto->chunk, proto->arity, proto->is_variadic, uv_count, proto->max_locals);
        push_temp_obj_root(reinterpret_cast<Obj**>(&instance));
        for (uint8_t i = 0; i < uv_count; ++i) {
          uint8_t is_local = *ip++;
          uint8_t index = *ip++;
          if (is_local) {
            Value &slot_ref = f.stack[frame->stack_base + index];
            if (!Heap::is_upvalue(slot_ref)) {
              ObjUpvalue *box = heap.allocate<ObjUpvalue>(slot_ref);
              slot_ref = Value::from_ptr(box);
            }
            instance->env[i] = slot_ref;
          } else {
            instance->env[i] = frame->closure->env[index];
          }
        }
        pop_temp_obj_root();
        f.push(Value::from_ptr(instance));
        break;
      }

      case OP_YIELD: {
        frame->ip = ip;
        f.state = Fiber::State::Suspended;
        return StepResult::Yielded;
      }

      case OP_PUSH_WINDER: {
        f.winders.push_back(f.pop());
        break;
      }

      case OP_POP_WINDER: {
        // Back onto the operand stack; the compiler follows this with
        // OP_CALL 0 / OP_POP so the cleanup runs inline in this very
        // dispatch loop (yield-legal), its result discarded, the
        // protected body's value left on top.
        assert(!f.winders.empty() && "OP_POP_WINDER with no pending winder");
        f.push(f.winders.back());
        f.winders.pop_back();
        break;
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

  // Fell off the counted loop: the opt-in instruction cap was exhausted.
  // This is preemption and is reported as such — for years-of-one-session
  // this returned Yielded, indistinguishable from OP_YIELD, which meant
  // an instruction budget could silently interleave fibers at boundaries
  // they never chose. (Unreachable when max_instructions == UNBOUNDED.)
  frame->ip = ip;
  f.state = Fiber::State::Suspended;
  return StepResult::Preempted;
}

// Step all active background fibers — see the declaration's comment for
// the shared-deadline and exclusive-resume (preempted_fiber) policies.
size_t VM::step_all_active_fibers(size_t instructions_per_fiber,
                                  std::chrono::milliseconds wall_clock_budget) {
  auto deadline = (wall_clock_budget == std::chrono::milliseconds::max())
                      ? NO_DEADLINE
                      : std::chrono::steady_clock::now() + wall_clock_budget;
  size_t preempted_count = 0;

  // A fiber the deadline cut off mid-flight last call is owed an
  // exclusive resume: nothing else may step until it reaches its own
  // yield, so no sibling ever observes its half-finished work.
  if (preempted_fiber) {
    auto it = std::find(active_fibers.begin(), active_fibers.end(), preempted_fiber);
    if (it == active_fibers.end()) {
      preempted_fiber = nullptr; // it died elsewhere; nothing owed
    } else {
      Fiber *f = preempted_fiber;
      StepResult res = step_fiber(*f, instructions_per_fiber, deadline);
      if (res == StepResult::Preempted) {
        return 1; // still not at a yield point — keep its exclusivity
      }
      preempted_fiber = nullptr;
      if (res == StepResult::Completed || res == StepResult::Error) {
        active_fibers.erase(std::find(active_fibers.begin(), active_fibers.end(), f));
        delete f;
      }
      // it yielded (or finished) — the round may proceed below
    }
  }

  for (size_t i = 0; i < active_fibers.size(); ) {
    // Deadline already spent: don't start another fiber against it
    // (its first check is 1024 instructions in — it would overshoot).
    if (deadline != NO_DEADLINE && std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    Fiber *f = active_fibers[i];
    StepResult res = step_fiber(*f, instructions_per_fiber, deadline);
    if (res == StepResult::Completed || res == StepResult::Error) {
      active_fibers.erase(active_fibers.begin() + i);
      delete f;
    } else if (res == StepResult::Preempted) {
      // First preemption ends the round: this fiber takes the exclusive-
      // resume slot, and no sibling may step past it — under an
      // instruction cap this serializes progress into atomic inter-yield
      // sections rather than interleaving mid-flight fibers (the old,
      // unsound behavior), and under a deadline the budget is spent
      // anyway.
      ++preempted_count;
      preempted_fiber = f;
      break;
    } else {
      ++i;
    }
  }
  return preempted_count;
}

// Case-insensitive three-way compare, shared by the string-ci*? family.
// A free function rather than a local lambda: NativeSubrFn is a plain
// function pointer, so the subr lambdas below must stay capture-less —
// calling this by name costs nothing, capturing it would forbid the
// pointer conversion make_subr needs.
static int ci_compare(std::string_view a, std::string_view b) {
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    unsigned char ca = std::tolower(static_cast<unsigned char>(a[i]));
    unsigned char cb = std::tolower(static_cast<unsigned char>(b[i]));
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  if (a.size() == b.size()) return 0;
  return a.size() < b.size() ? -1 : 1;
}

// Resolves the optional trailing port argument I/O primitives take
// (e.g. (display obj [port]), (read-char [port])) — falls back to the
// VM's current in/out port when absent, ignores a wrong-direction or
// closed port rather than crashing on it.
static std::ostream *resolve_out(VM &vm, uint32_t argc, Value *args, uint32_t port_arg_index) {
  if (argc > port_arg_index && Heap::is_port(args[port_arg_index])) {
    ObjPort *p = args[port_arg_index].as_ptr<ObjPort>();
    if (!p->is_input && !p->closed && p->out) return p->out;
  }
  return &vm.out_stream();
}

static std::istream *resolve_in(VM &vm, uint32_t argc, Value *args, uint32_t port_arg_index) {
  if (argc > port_arg_index && Heap::is_port(args[port_arg_index])) {
    ObjPort *p = args[port_arg_index].as_ptr<ObjPort>();
    if (p->is_input && !p->closed && p->in) return p->in;
  }
  return &vm.in_stream();
}

// Formats a raised value for human consumption — used both to build an
// error-object's own display (indirectly, via error's construction
// below) and as RaiseEscape's fallback message, for whatever ends up
// seeing an uncaught raise (main.cpp's top-level catch, vx_wasm.cpp's).
// An error-object formats the way `error` always has (message, then
// each irritant write-formatted); any other raised value — (raise 'foo)
// is perfectly legal R7RS — just shows what it is.
static std::string format_raised_value(const VM &vm, Value v) {
  if (Heap::is_error_object(v)) {
    ObjVector *ov = v.as_ptr<ObjVector>();
    std::ostringstream oss;
    vm.display_value(ov->get(0), oss);
    for (uint32_t i = 1; i < ov->size; ++i) {
      oss << " " << vm.format_value(ov->get(i));
    }
    return "[Scheme Error] " + oss.str();
  }
  return "uncaught exception: " + vm.format_value(v);
}

// Tries filename, then testcases/filename, ../testcases/filename,
// ../filename — the same search order `load` already used, so scripts
// run from either the repo root or src/ can find testcases/ files.
static std::unique_ptr<std::ifstream> open_input_with_fallback(const std::string &filename) {
  auto ifs = std::make_unique<std::ifstream>(filename);
  if (!ifs->is_open()) ifs->open("testcases/" + filename);
  if (!ifs->is_open()) ifs->open("../testcases/" + filename);
  if (!ifs->is_open()) ifs->open("../" + filename);
  if (!ifs->is_open()) return nullptr;
  return ifs;
}

// Builtin primitive registration
void VM::init_primitives() {
  // 1. Math & Arithmetic
  auto subr_add = [](VM &, uint32_t argc, Value *args) -> Value {
    if (argc == 0) return Value::from_int(0);
    bool all_int = true;
    int64_t isum = 0;
    double dsum = 0.0;
    for (uint32_t i = 0; i < argc; ++i) {
      if (!args[i].is_int()) all_int = false;
      if (all_int) isum += args[i].as_int();
      dsum += args[i].as_real();
    }
    if (all_int) {
      if (isum >= INT32_MIN && isum <= INT32_MAX) return Value::from_int(static_cast<int32_t>(isum));
      return Value::from_double(static_cast<double>(isum));
    }
    return Value::from_double(dsum);
  };
  def_global("+", heap.make_subr("+", subr_add, 0, UINT32_MAX));

  auto subr_sub = [](VM &, uint32_t argc, Value *args) -> Value {
    if (argc == 1) {
      if (args[0].is_int()) {
        int64_t v = -static_cast<int64_t>(args[0].as_int());
        if (v >= INT32_MIN && v <= INT32_MAX) return Value::from_int(static_cast<int32_t>(v));
        return Value::from_double(static_cast<double>(v));
      }
      return Value::from_double(-args[0].as_real());
    }
    bool all_int = true;
    for (uint32_t i = 0; i < argc; ++i) {
      if (!args[i].is_int()) { all_int = false; break; }
    }
    if (all_int) {
      int64_t isum = args[0].as_int();
      for (uint32_t i = 1; i < argc; ++i) isum -= args[i].as_int();
      if (isum >= INT32_MIN && isum <= INT32_MAX) return Value::from_int(static_cast<int32_t>(isum));
      return Value::from_double(static_cast<double>(isum));
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
    int64_t iprod = 1;
    double dprod = 1.0;
    for (uint32_t i = 0; i < argc; ++i) {
      if (!args[i].is_int()) all_int = false;
      if (all_int) iprod *= args[i].as_int();
      dprod *= args[i].as_real();
    }
    if (all_int) {
      if (iprod >= INT32_MIN && iprod <= INT32_MAX) return Value::from_int(static_cast<int32_t>(iprod));
      return Value::from_double(static_cast<double>(iprod));
    }
    return Value::from_double(dprod);
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
    int64_t r = a % b;
    if (r >= INT32_MIN && r <= INT32_MAX) return Value::from_int(static_cast<int32_t>(r));
    return Value::from_double(static_cast<double>(r));
  };
  def_global("remainder", heap.make_subr("remainder", subr_rem, 2, 2));
  def_global("modulo", heap.make_subr("modulo", subr_rem, 2, 2));

  // Comparisons — R4RS numeric comparisons are N-ary: (< a1 a2 a3 ...) holds
  // iff every consecutive pair satisfies the relation.
  auto subr_num_eq = [](VM &, uint32_t argc, Value *args) -> Value {
    for (uint32_t i = 1; i < argc; ++i) {
      if (!(args[i - 1].as_real() == args[i].as_real())) return Value::boolean_false();
    }
    return Value::boolean_true();
  };
  def_global("=", heap.make_subr("=", subr_num_eq, 1, UINT32_MAX));

  auto subr_lt = [](VM &, uint32_t argc, Value *args) -> Value {
    for (uint32_t i = 1; i < argc; ++i) {
      if (!(args[i - 1].as_real() < args[i].as_real())) return Value::boolean_false();
    }
    return Value::boolean_true();
  };
  def_global("<", heap.make_subr("<", subr_lt, 1, UINT32_MAX));

  auto subr_le = [](VM &, uint32_t argc, Value *args) -> Value {
    for (uint32_t i = 1; i < argc; ++i) {
      if (!(args[i - 1].as_real() <= args[i].as_real())) return Value::boolean_false();
    }
    return Value::boolean_true();
  };
  def_global("<=", heap.make_subr("<=", subr_le, 1, UINT32_MAX));

  auto subr_gt = [](VM &, uint32_t argc, Value *args) -> Value {
    for (uint32_t i = 1; i < argc; ++i) {
      if (!(args[i - 1].as_real() > args[i].as_real())) return Value::boolean_false();
    }
    return Value::boolean_true();
  };
  def_global(">", heap.make_subr(">", subr_gt, 1, UINT32_MAX));

  auto subr_ge = [](VM &, uint32_t argc, Value *args) -> Value {
    for (uint32_t i = 1; i < argc; ++i) {
      if (!(args[i - 1].as_real() >= args[i].as_real())) return Value::boolean_false();
    }
    return Value::boolean_true();
  };
  def_global(">=", heap.make_subr(">=", subr_ge, 1, UINT32_MAX));

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

  // floor/ceiling/round preserve exactness per R4RS (inexact in, inexact
  // out) — matching the existing truncate below, which already got this
  // right. round specifically uses round-half-to-even ("banker's
  // rounding"), same as R4RS through R7RS all specify, and the same
  // convention IEEE 754's default rounding mode uses: std::nearbyint
  // (unlike std::round, which always rounds half away from zero) honors
  // the current rounding mode, which defaults to round-to-nearest-even.
  auto subr_floor = [](VM &, uint32_t, Value *args) -> Value {
    if (args[0].is_int()) return args[0];
    return Value::from_double(std::floor(args[0].as_real()));
  };
  def_global("floor", heap.make_subr("floor", subr_floor, 1, 1));

  auto subr_ceiling = [](VM &, uint32_t, Value *args) -> Value {
    if (args[0].is_int()) return args[0];
    return Value::from_double(std::ceil(args[0].as_real()));
  };
  def_global("ceiling", heap.make_subr("ceiling", subr_ceiling, 1, 1));

  auto subr_round = [](VM &, uint32_t, Value *args) -> Value {
    if (args[0].is_int()) return args[0];
    return Value::from_double(std::nearbyint(args[0].as_real()));
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

  // R4RS exactness contagion: if any argument is inexact, the result must
  // be inexact too, even when the winning value came from an exact
  // argument — (max 3.9 4) is 4.0, not exact 4.
  auto subr_max = [](VM &, uint32_t argc, Value *args) -> Value {
    if (argc == 0) return Value::from_double(-std::numeric_limits<double>::infinity());
    Value best = args[0];
    bool inexact = args[0].is_double();
    for (uint32_t i = 1; i < argc; ++i) {
      if (args[i].is_double()) inexact = true;
      if (args[i].as_real() > best.as_real()) best = args[i];
    }
    if (inexact && best.is_int()) return Value::from_double(best.as_real());
    return best;
  };
  def_global("max", heap.make_subr("max", subr_max, 1, UINT32_MAX));

  auto subr_min = [](VM &, uint32_t argc, Value *args) -> Value {
    if (argc == 0) return Value::from_double(std::numeric_limits<double>::infinity());
    Value best = args[0];
    bool inexact = args[0].is_double();
    for (uint32_t i = 1; i < argc; ++i) {
      if (args[i].is_double()) inexact = true;
      if (args[i].as_real() < best.as_real()) best = args[i];
    }
    if (inexact && best.is_int()) return Value::from_double(best.as_real());
    return best;
  };
  def_global("min", heap.make_subr("min", subr_min, 1, UINT32_MAX));

  auto subr_gcd = [](VM &, uint32_t argc, Value *args) -> Value {
    if (argc == 0) return Value::from_int(0);
    int64_t g = std::abs(args[0].is_int() ? args[0].as_int() : static_cast<int64_t>(args[0].as_real()));
    for (uint32_t i = 1; i < argc; ++i) {
      int64_t b = std::abs(args[i].is_int() ? args[i].as_int() : static_cast<int64_t>(args[i].as_real()));
      g = std::gcd(g, b);
    }
    return Value::from_int(static_cast<int32_t>(g));
  };
  def_global("gcd", heap.make_subr("gcd", subr_gcd, 0, UINT32_MAX));

  auto subr_lcm = [](VM &, uint32_t argc, Value *args) -> Value {
    if (argc == 0) return Value::from_int(1);
    int64_t l = std::abs(args[0].is_int() ? args[0].as_int() : static_cast<int64_t>(args[0].as_real()));
    for (uint32_t i = 1; i < argc; ++i) {
      int64_t b = std::abs(args[i].is_int() ? args[i].as_int() : static_cast<int64_t>(args[i].as_real()));
      l = std::lcm(l, b);
    }
    return Value::from_int(static_cast<int32_t>(l));
  };
  def_global("lcm", heap.make_subr("lcm", subr_lcm, 0, UINT32_MAX));

  def_global("sin", heap.make_subr("sin", [](VM &, uint32_t, Value *args) -> Value { return Value::from_double(std::sin(args[0].as_real())); }, 1, 1));
  def_global("cos", heap.make_subr("cos", [](VM &, uint32_t, Value *args) -> Value { return Value::from_double(std::cos(args[0].as_real())); }, 1, 1));
  def_global("tan", heap.make_subr("tan", [](VM &, uint32_t, Value *args) -> Value { return Value::from_double(std::tan(args[0].as_real())); }, 1, 1));
  def_global("asin", heap.make_subr("asin", [](VM &, uint32_t, Value *args) -> Value { return Value::from_double(std::asin(args[0].as_real())); }, 1, 1));
  def_global("acos", heap.make_subr("acos", [](VM &, uint32_t, Value *args) -> Value { return Value::from_double(std::acos(args[0].as_real())); }, 1, 1));
  def_global("atan", heap.make_subr("atan", [](VM &, uint32_t argc, Value *args) -> Value {
    if (argc == 1) return Value::from_double(std::atan(args[0].as_real()));
    return Value::from_double(std::atan2(args[0].as_real(), args[1].as_real()));
  }, 1, 2));
  def_global("expt", heap.make_subr("expt", [](VM &, uint32_t, Value *args) -> Value { return Value::from_double(std::pow(args[0].as_real(), args[1].as_real())); }, 2, 2));
  def_global("log", heap.make_subr("log", [](VM &, uint32_t, Value *args) -> Value { return Value::from_double(std::log(args[0].as_real())); }, 1, 1));

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

#define DEF_CXR2(name, op1, op2) \
  def_global(#name, heap.make_subr(#name, [](VM &, uint32_t, Value *args) -> Value { \
    Value v = args[0]; \
    if (!Heap::is_cons(v)) return Value::nil(); \
    v = Heap::op2(v); \
    if (!Heap::is_cons(v)) return Value::nil(); \
    return Heap::op1(v); \
  }, 1, 1));

#define DEF_CXR3(name, op1, op2, op3) \
  def_global(#name, heap.make_subr(#name, [](VM &, uint32_t, Value *args) -> Value { \
    Value v = args[0]; \
    if (!Heap::is_cons(v)) return Value::nil(); \
    v = Heap::op3(v); \
    if (!Heap::is_cons(v)) return Value::nil(); \
    v = Heap::op2(v); \
    if (!Heap::is_cons(v)) return Value::nil(); \
    return Heap::op1(v); \
  }, 1, 1));

#define DEF_CXR4(name, op1, op2, op3, op4) \
  def_global(#name, heap.make_subr(#name, [](VM &, uint32_t, Value *args) -> Value { \
    Value v = args[0]; \
    if (!Heap::is_cons(v)) return Value::nil(); \
    v = Heap::op4(v); \
    if (!Heap::is_cons(v)) return Value::nil(); \
    v = Heap::op3(v); \
    if (!Heap::is_cons(v)) return Value::nil(); \
    v = Heap::op2(v); \
    if (!Heap::is_cons(v)) return Value::nil(); \
    return Heap::op1(v); \
  }, 1, 1));

  DEF_CXR2(caar, car, car)
  DEF_CXR2(cadr, car, cdr)
  DEF_CXR2(cdar, cdr, car)
  DEF_CXR2(cddr, cdr, cdr)

  DEF_CXR3(caaar, car, car, car)
  DEF_CXR3(caadr, car, car, cdr)
  DEF_CXR3(cadar, car, cdr, car)
  DEF_CXR3(caddr, car, cdr, cdr)
  DEF_CXR3(cdaar, cdr, car, car)
  DEF_CXR3(cdadr, cdr, car, cdr)
  DEF_CXR3(cddar, cdr, cdr, car)
  DEF_CXR3(cdddr, cdr, cdr, cdr)

  DEF_CXR4(caaaar, car, car, car, car)
  DEF_CXR4(caaadr, car, car, car, cdr)
  DEF_CXR4(caadar, car, car, cdr, car)
  DEF_CXR4(caaddr, car, car, cdr, cdr)
  DEF_CXR4(cadaar, car, cdr, car, car)
  DEF_CXR4(cadadr, car, cdr, car, cdr)
  DEF_CXR4(caddar, car, cdr, cdr, car)
  DEF_CXR4(cadddr, car, cdr, cdr, cdr)
  DEF_CXR4(cdaaar, cdr, car, car, car)
  DEF_CXR4(cdaadr, cdr, car, car, cdr)
  DEF_CXR4(cdadar, cdr, car, cdr, car)
  DEF_CXR4(cdaddr, cdr, car, cdr, cdr)
  DEF_CXR4(cddaar, cdr, cdr, car, car)
  DEF_CXR4(cddadr, cdr, cdr, car, cdr)
  DEF_CXR4(cdddar, cdr, cdr, cdr, car)
  DEF_CXR4(cddddr, cdr, cdr, cdr, cdr)

#undef DEF_CXR2
#undef DEF_CXR3
#undef DEF_CXR4

  auto subr_list = [](VM &vm, uint32_t argc, Value *args) -> Value {
    Value res = Value::nil();
    vm.push_temp_root(&res);
    for (int i = static_cast<int>(argc) - 1; i >= 0; --i) {
      res = vm.heap.cons(args[i], res);
    }
    vm.pop_temp_root();
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
    vm.push_temp_root(&res);
    while (Heap::is_cons(cur)) {
      res = vm.heap.cons(Heap::car(cur), res);
      cur = Heap::cdr(cur);
    }
    vm.pop_temp_root();
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
    vm.push_temp_root(&res);
    for (auto it = all_items.rbegin(); it != all_items.rend(); ++it) {
      res = vm.heap.cons(*it, res);
    }
    vm.pop_temp_root();
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
      uint32_t n = static_cast<uint32_t>(flat_args.size());
      if (n < subr->min_args || n > subr->max_args) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message =
            "[VM Error] " + std::string(subr->name) + ": wrong number of arguments";
        return Value::unspecified();
      }
      return vm.call_subr(subr, n, flat_args.data());
    }
    if (Heap::is_closure(fn)) {
      return vm.call_closure(fn.as_ptr<ObjClosure>(), flat_args);
    }
    return Value::unspecified();
  };
  def_global("apply", heap.make_subr("apply", subr_apply, 2, UINT32_MAX));

  auto subr_values = [](VM &vm, uint32_t argc, Value *args) -> Value {
    // A single value is returned bare, not wrapped — see
    // Heap::make_multivalue's comment. Ordinary code calling an ordinary
    // procedure (which implicitly "returns one value") never needs to
    // know values exists at all.
    if (argc == 1) return args[0];
    return vm.heap.make_multivalue(std::vector<Value>(args, args + argc));
  };
  def_global("values", heap.make_subr("values", subr_values, 0, UINT32_MAX));

  auto subr_call_with_values = [](VM &vm, uint32_t, Value *args) -> Value {
    Value producer = args[0];
    Value consumer = args[1];
    Value result = Value::unspecified();
    vm.push_temp_root(&result);
    if (Heap::is_subr(producer)) {
      result = vm.call_subr(producer.as_ptr<ObjSubr>(), 0, nullptr);
    } else if (Heap::is_closure(producer)) {
      result = vm.call_closure(producer.as_ptr<ObjClosure>(), {});
    }
    std::vector<Value> consumer_args;
    if (Heap::is_multivalue(result)) {
      ObjVector *ov = result.as_ptr<ObjVector>();
      consumer_args.assign(ov->data, ov->data + ov->size);
    } else {
      consumer_args.push_back(result);
    }
    vm.pop_temp_root();
    if (Heap::is_subr(consumer)) {
      ObjSubr *subr = consumer.as_ptr<ObjSubr>();
      uint32_t n = static_cast<uint32_t>(consumer_args.size());
      if (n < subr->min_args || n > subr->max_args) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message =
            "[VM Error] call-with-values: consumer expected a different number of values, got " + std::to_string(n);
        return Value::unspecified();
      }
      return vm.call_subr(subr, n, consumer_args.data());
    }
    if (Heap::is_closure(consumer)) {
      return vm.call_closure(consumer.as_ptr<ObjClosure>(), consumer_args);
    }
    return Value::unspecified();
  };
  def_global("call-with-values", heap.make_subr("call-with-values", subr_call_with_values, 2, 2));

  auto subr_map = [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (argc < 2) return Value::nil();
    Value fn = args[0];
    // Builds the result in reverse call-order by consing onto `res`
    // immediately after each call, rather than collecting raw call outputs
    // into a std::vector first — an output value is a fresh allocation
    // reachable from nothing else, so parking several of them in an
    // unrooted C++ container while further calls run (and might GC) would
    // leave the earlier ones to be collected out from under it. `res`
    // itself stays protected via push_temp_root the whole time, and
    // mark_roots re-reads it fresh on every collection, so it always sees
    // wherever the chain-in-progress currently is.
    if (argc == 2) {
      Value cur = args[1];
      Value res = Value::nil();
      vm.push_temp_root(&res);
      while (Heap::is_cons(cur)) {
        Value elem = Heap::car(cur);
        Value out = Value::nil();
        if (Heap::is_subr(fn)) {
          out = vm.call_subr(fn.as_ptr<ObjSubr>(), 1, &elem);
        } else if (Heap::is_closure(fn)) {
          out = vm.call_closure(fn.as_ptr<ObjClosure>(), {elem});
        }
        res = vm.heap.cons(out, res);
        cur = Heap::cdr(cur);
      }
      vm.pop_temp_root();
      Value forward = Value::nil();
      vm.push_temp_root(&forward);
      while (Heap::is_cons(res)) {
        forward = vm.heap.cons(Heap::car(res), forward);
        res = Heap::cdr(res);
      }
      vm.pop_temp_root();
      return forward;
    }
    // N-ary map: (map proc list1 list2 ...)
    std::vector<Value> lists(args + 1, args + argc);
    Value res = Value::nil();
    vm.push_temp_root(&res);
    while (true) {
      for (Value l : lists) {
        if (!Heap::is_cons(l)) goto done_map;
      }
      std::vector<Value> step_args;
      step_args.reserve(lists.size());
      for (size_t i = 0; i < lists.size(); ++i) {
        step_args.push_back(Heap::car(lists[i]));
        lists[i] = Heap::cdr(lists[i]);
      }
      Value out = Value::nil();
      if (Heap::is_subr(fn)) {
        out = vm.call_subr(fn.as_ptr<ObjSubr>(), static_cast<uint32_t>(step_args.size()), step_args.data());
      } else if (Heap::is_closure(fn)) {
        out = vm.call_closure(fn.as_ptr<ObjClosure>(), step_args);
      }
      res = vm.heap.cons(out, res);
    }
  done_map:
    vm.pop_temp_root();
    Value forward = Value::nil();
    vm.push_temp_root(&forward);
    while (Heap::is_cons(res)) {
      forward = vm.heap.cons(Heap::car(res), forward);
      res = Heap::cdr(res);
    }
    vm.pop_temp_root();
    return forward;
  };
  def_global("map", heap.make_subr("map", subr_map, 2, UINT32_MAX));

  auto subr_for_each = [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (argc < 2) return Value::unspecified();
    Value fn = args[0];
    if (argc == 2) {
      Value cur = args[1];
      while (Heap::is_cons(cur)) {
        Value elem = Heap::car(cur);
        if (Heap::is_subr(fn)) {
          vm.call_subr(fn.as_ptr<ObjSubr>(), 1, &elem);
        } else if (Heap::is_closure(fn)) {
          vm.call_closure(fn.as_ptr<ObjClosure>(), {elem});
        }
        cur = Heap::cdr(cur);
      }
      // A proper list's walk always ends at nil; anything else (a vector,
      // an improper/dotted tail, ...) means the argument was never a list
      // to begin with, rather than "an empty one" — for-each only walks
      // lists (see vector-for-each/string-for-each for the vector/string
      // equivalents), so make that loud instead of quietly doing nothing.
      if (!cur.is_nil()) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] for-each: not a proper list, got " + vm.format_value(args[1]);
      }
      return Value::unspecified();
    }
    // N-ary for-each
    std::vector<Value> lists(args + 1, args + argc);
    while (true) {
      bool any_ended = false;
      for (Value l : lists) {
        if (!Heap::is_cons(l)) {
          any_ended = true;
          if (!l.is_nil()) {
            vm.current_fiber->state = Fiber::State::Error;
            vm.current_fiber->error_message = "[VM Error] for-each: not a proper list, got " + vm.format_value(l);
          }
        }
      }
      if (any_ended) return Value::unspecified();
      std::vector<Value> step_args;
      step_args.reserve(lists.size());
      for (size_t i = 0; i < lists.size(); ++i) {
        step_args.push_back(Heap::car(lists[i]));
        lists[i] = Heap::cdr(lists[i]);
      }
      if (Heap::is_subr(fn)) {
        vm.call_subr(fn.as_ptr<ObjSubr>(), static_cast<uint32_t>(step_args.size()), step_args.data());
      } else if (Heap::is_closure(fn)) {
        vm.call_closure(fn.as_ptr<ObjClosure>(), step_args);
      }
    }
    return Value::unspecified();
  };
  def_global("for-each", heap.make_subr("for-each", subr_for_each, 2, UINT32_MAX));

  def_global("list-ref", heap.make_subr("list-ref", [](VM &vm, uint32_t, Value *args) -> Value {
    Value cur = args[0];
    int32_t k = args[1].as_int();
    while (k > 0 && Heap::is_cons(cur)) {
      cur = Heap::cdr(cur);
      --k;
    }
    if (k == 0 && Heap::is_cons(cur)) return Heap::car(cur);
    if (vm.current_fiber) {
      vm.current_fiber->state = Fiber::State::Error;
      vm.current_fiber->error_message = "[VM Error] list-ref: index out of bounds";
    }
    return Value::unspecified();
  }, 2, 2));

  def_global("list-tail", heap.make_subr("list-tail", [](VM &, uint32_t, Value *args) -> Value {
    Value cur = args[0];
    int32_t k = args[1].as_int();
    while (k > 0 && Heap::is_cons(cur)) {
      cur = Heap::cdr(cur);
      --k;
    }
    return cur;
  }, 2, 2));

  def_global("read", heap.make_subr("read", [](VM &vm, uint32_t argc, Value *args) -> Value {
    std::istream *is = resolve_in(vm, argc, args, 0);
    std::streampos start = is->tellg();
    std::string s;
    char c;
    while (is->get(c)) s += c;
    if (s.empty()) return Value::eof_obj();
    // Covers quote_materialize below too, not just the read itself — it
    // recursively reallocates the whole parsed tree (every cons cell, not
    // just vector/map-shaped parts), and unlike the compiler's use of the
    // same function (implicitly covered by compile_top_level's GCGuard),
    // nothing here otherwise roots the freshly-read `form` between when
    // the reader's own internal guard releases and quote_materialize
    // finishes walking it.
    GCGuard guard(vm.heap);
    Reader reader(vm, s);
    Value form = reader.read_form();
    // Only the reader's own single form should be consumed from the
    // stream — rewind past whatever it actually read (not the whole
    // remaining stream, which was just slurped above to give it
    // something to parse from) so a subsequent read continues from the
    // right place instead of hitting EOF immediately.
    is->clear();
    is->seekg(start + static_cast<std::streamoff>(reader.position()));
    // read has no compilation pass of its own (unlike code, which goes
    // through the compiler's quote/quasiquote handling), so without this
    // a [...]/{...} in the input would come back as the inert
    // (vector ...)/(hash-map ...) call form the reader produces rather
    // than an actual vector/map — see quote_materialize's comment.
    return quote_materialize(vm, form);
  }, 0, 1));

  auto subr_filter = [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (argc != 2) return Value::nil();
    Value fn = args[0];
    Value cur = args[1];
    std::vector<Value> results;
    while (Heap::is_cons(cur)) {
      Value elem = Heap::car(cur);
      Value match = Value::boolean_false();
      if (Heap::is_subr(fn)) {
        match = vm.call_subr(fn.as_ptr<ObjSubr>(), 1, &elem);
      } else if (Heap::is_closure(fn)) {
        match = vm.call_closure(fn.as_ptr<ObjClosure>(), {elem});
      }
      if (match.is_bool() ? match.as_bool() : !match.is_nil()) {
        results.push_back(elem);
      }
      cur = Heap::cdr(cur);
    }
    Value res = Value::nil();
    vm.push_temp_root(&res);
    for (auto it = results.rbegin(); it != results.rend(); ++it) {
      res = vm.heap.cons(*it, res);
    }
    vm.pop_temp_root();
    return res;
  };
  def_global("filter", heap.make_subr("filter", subr_filter, 2, 2));

  auto subr_null_p = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_nil());
  };
  def_global("null?", heap.make_subr("null?", subr_null_p, 1, 1));

  auto subr_pair_p = [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_cons(args[0]));
  };
  def_global("pair?", heap.make_subr("pair?", subr_pair_p, 1, 1));

  def_global("boolean?", heap.make_subr("boolean?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_bool());
  }, 1, 1));

  def_global("number?", heap.make_subr("number?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_int() || args[0].is_double());
  }, 1, 1));

  def_global("complex?", heap.make_subr("complex?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_int() || args[0].is_double());
  }, 1, 1));

  def_global("rational?", heap.make_subr("rational?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_int() || args[0].is_double());
  }, 1, 1));

  def_global("real?", heap.make_subr("real?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_int() || args[0].is_double());
  }, 1, 1));

  def_global("integer?", heap.make_subr("integer?", [](VM &, uint32_t, Value *args) -> Value {
    if (args[0].is_int()) return Value::boolean_true();
    if (args[0].is_double()) {
      double d = args[0].as_double();
      return Value::from_bool(!std::isnan(d) && !std::isinf(d) && std::floor(d) == d);
    }
    return Value::boolean_false();
  }, 1, 1));

  def_global("exact?", heap.make_subr("exact?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_int());
  }, 1, 1));

  def_global("inexact?", heap.make_subr("inexact?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_double());
  }, 1, 1));

  // R7RS additions: exactness/integer-ness is fully determined by the
  // int/double tag in this system (no separate rational/bignum tower),
  // so exact-integer? is just is_int() — an int can't help being both.
  def_global("exact-integer?", heap.make_subr("exact-integer?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_int());
  }, 1, 1));

  def_global("nan?", heap.make_subr("nan?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_double() && std::isnan(args[0].as_double()));
  }, 1, 1));

  def_global("infinite?", heap.make_subr("infinite?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_double() && std::isinf(args[0].as_double()));
  }, 1, 1));

  def_global("finite?", heap.make_subr("finite?", [](VM &, uint32_t, Value *args) -> Value {
    if (args[0].is_int()) return Value::boolean_true();
    return Value::from_bool(args[0].is_double() && std::isfinite(args[0].as_double()));
  }, 1, 1));

  def_global("square", heap.make_subr("square", [](VM &, uint32_t, Value *args) -> Value {
    if (args[0].is_int()) {
      int64_t v = args[0].as_int();
      int64_t sq = v * v;
      if (sq >= INT32_MIN && sq <= INT32_MAX) return Value::from_int(static_cast<int32_t>(sq));
      return Value::from_double(static_cast<double>(sq));
    }
    double d = args[0].as_real();
    return Value::from_double(d * d);
  }, 1, 1));

  def_global("string?", heap.make_subr("string?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_string(args[0]));
  }, 1, 1));

  def_global("char?", heap.make_subr("char?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_char());
  }, 1, 1));

  def_global("symbol?", heap.make_subr("symbol?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_symbol());
  }, 1, 1));

  def_global("vector?", heap.make_subr("vector?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_vector(args[0]));
  }, 1, 1));

  def_global("keyword?", heap.make_subr("keyword?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_keyword());
  }, 1, 1));

  def_global("map?", heap.make_subr("map?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_map(args[0]));
  }, 1, 1));

  def_global("list?", heap.make_subr("list?", [](VM &, uint32_t, Value *args) -> Value {
    Value slow = args[0];
    Value fast = args[0];
    while (Heap::is_cons(fast)) {
      fast = Heap::cdr(fast);
      if (fast.is_nil()) return Value::boolean_true();
      if (!Heap::is_cons(fast)) return Value::boolean_false();
      fast = Heap::cdr(fast);
      slow = Heap::cdr(slow);
      if (fast.raw == slow.raw) return Value::boolean_false(); // cycle
    }
    return Value::from_bool(fast.is_nil());
  }, 1, 1));

  def_global("char=?", heap.make_subr("char=?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_char() && args[1].is_char() && args[0].as_char() == args[1].as_char());
  }, 2, 2));

  def_global("char<?", heap.make_subr("char<?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_char() && args[1].is_char() && args[0].as_char() < args[1].as_char());
  }, 2, 2));

  def_global("char<=?", heap.make_subr("char<=?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_char() && args[1].is_char() && args[0].as_char() <= args[1].as_char());
  }, 2, 2));

  def_global("char>?", heap.make_subr("char>?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_char() && args[1].is_char() && args[0].as_char() > args[1].as_char());
  }, 2, 2));

  def_global("char>=?", heap.make_subr("char>=?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_char() && args[1].is_char() && args[0].as_char() >= args[1].as_char());
  }, 2, 2));

  def_global("char-ci=?", heap.make_subr("char-ci=?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_char() && args[1].is_char() && std::tolower(static_cast<unsigned char>(args[0].as_char())) == std::tolower(static_cast<unsigned char>(args[1].as_char())));
  }, 2, 2));

  def_global("char-ci<?", heap.make_subr("char-ci<?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_char() && args[1].is_char() && std::tolower(static_cast<unsigned char>(args[0].as_char())) < std::tolower(static_cast<unsigned char>(args[1].as_char())));
  }, 2, 2));

  def_global("char-ci<=?", heap.make_subr("char-ci<=?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_char() && args[1].is_char() && std::tolower(static_cast<unsigned char>(args[0].as_char())) <= std::tolower(static_cast<unsigned char>(args[1].as_char())));
  }, 2, 2));

  def_global("char-ci>?", heap.make_subr("char-ci>?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_char() && args[1].is_char() && std::tolower(static_cast<unsigned char>(args[0].as_char())) > std::tolower(static_cast<unsigned char>(args[1].as_char())));
  }, 2, 2));

  def_global("char-ci>=?", heap.make_subr("char-ci>=?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_char() && args[1].is_char() && std::tolower(static_cast<unsigned char>(args[0].as_char())) >= std::tolower(static_cast<unsigned char>(args[1].as_char())));
  }, 2, 2));

  def_global("char-alphabetic?", heap.make_subr("char-alphabetic?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_char() && std::isalpha(static_cast<unsigned char>(args[0].as_char())));
  }, 1, 1));

  def_global("char-numeric?", heap.make_subr("char-numeric?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_char() && std::isdigit(static_cast<unsigned char>(args[0].as_char())));
  }, 1, 1));

  def_global("char-whitespace?", heap.make_subr("char-whitespace?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_char() && std::isspace(static_cast<unsigned char>(args[0].as_char())));
  }, 1, 1));

  def_global("char-upper-case?", heap.make_subr("char-upper-case?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_char() && std::isupper(static_cast<unsigned char>(args[0].as_char())));
  }, 1, 1));

  def_global("char-lower-case?", heap.make_subr("char-lower-case?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_char() && std::islower(static_cast<unsigned char>(args[0].as_char())));
  }, 1, 1));

  def_global("char-upcase", heap.make_subr("char-upcase", [](VM &, uint32_t, Value *args) -> Value {
    if (!args[0].is_char()) return args[0];
    return Value::from_char(static_cast<char>(std::toupper(static_cast<unsigned char>(args[0].as_char()))));
  }, 1, 1));

  def_global("char-downcase", heap.make_subr("char-downcase", [](VM &, uint32_t, Value *args) -> Value {
    if (!args[0].is_char()) return args[0];
    return Value::from_char(static_cast<char>(std::tolower(static_cast<unsigned char>(args[0].as_char()))));
  }, 1, 1));

  def_global("char->integer", heap.make_subr("char->integer", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_int(static_cast<uint8_t>(args[0].as_char()));
  }, 1, 1));

  def_global("integer->char", heap.make_subr("integer->char", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_char(static_cast<char>(args[0].as_int()));
  }, 1, 1));

  def_global("string=?", heap.make_subr("string=?", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0]) || !Heap::is_string(args[1])) return Value::boolean_false();
    return Value::from_bool(args[0].as_ptr<ObjString>()->view() == args[1].as_ptr<ObjString>()->view());
  }, 2, 2));

  def_global("string<?", heap.make_subr("string<?", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0]) || !Heap::is_string(args[1])) return Value::boolean_false();
    return Value::from_bool(args[0].as_ptr<ObjString>()->view() < args[1].as_ptr<ObjString>()->view());
  }, 2, 2));

  def_global("string<=?", heap.make_subr("string<=?", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0]) || !Heap::is_string(args[1])) return Value::boolean_false();
    return Value::from_bool(args[0].as_ptr<ObjString>()->view() <= args[1].as_ptr<ObjString>()->view());
  }, 2, 2));

  def_global("string>?", heap.make_subr("string>?", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0]) || !Heap::is_string(args[1])) return Value::boolean_false();
    return Value::from_bool(args[0].as_ptr<ObjString>()->view() > args[1].as_ptr<ObjString>()->view());
  }, 2, 2));

  def_global("string>=?", heap.make_subr("string>=?", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0]) || !Heap::is_string(args[1])) return Value::boolean_false();
    return Value::from_bool(args[0].as_ptr<ObjString>()->view() >= args[1].as_ptr<ObjString>()->view());
  }, 2, 2));

  def_global("string-ci=?", heap.make_subr("string-ci=?", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0]) || !Heap::is_string(args[1])) return Value::boolean_false();
    return Value::from_bool(ci_compare(args[0].as_ptr<ObjString>()->view(), args[1].as_ptr<ObjString>()->view()) == 0);
  }, 2, 2));

  def_global("string-ci<?", heap.make_subr("string-ci<?", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0]) || !Heap::is_string(args[1])) return Value::boolean_false();
    return Value::from_bool(ci_compare(args[0].as_ptr<ObjString>()->view(), args[1].as_ptr<ObjString>()->view()) < 0);
  }, 2, 2));

  def_global("string-ci<=?", heap.make_subr("string-ci<=?", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0]) || !Heap::is_string(args[1])) return Value::boolean_false();
    return Value::from_bool(ci_compare(args[0].as_ptr<ObjString>()->view(), args[1].as_ptr<ObjString>()->view()) <= 0);
  }, 2, 2));

  def_global("string-ci>?", heap.make_subr("string-ci>?", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0]) || !Heap::is_string(args[1])) return Value::boolean_false();
    return Value::from_bool(ci_compare(args[0].as_ptr<ObjString>()->view(), args[1].as_ptr<ObjString>()->view()) > 0);
  }, 2, 2));

  def_global("string-ci>=?", heap.make_subr("string-ci>=?", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0]) || !Heap::is_string(args[1])) return Value::boolean_false();
    return Value::from_bool(ci_compare(args[0].as_ptr<ObjString>()->view(), args[1].as_ptr<ObjString>()->view()) >= 0);
  }, 2, 2));

  // Display / Output — all take an optional trailing port argument,
  // defaulting to the VM's current output port (ordinarily stdout,
  // unless within-output-to-file has temporarily rebound it).
  def_global("display", heap.make_subr("display", [](VM &vm, uint32_t argc, Value *args) -> Value {
    vm.display_value(args[0], *resolve_out(vm, argc, args, 1));
    return Value::unspecified();
  }, 1, 2));

  def_global("newline", heap.make_subr("newline", [](VM &vm, uint32_t argc, Value *args) -> Value {
    *resolve_out(vm, argc, args, 0) << std::endl;
    return Value::unspecified();
  }, 0, 1));

  def_global("write-char", heap.make_subr("write-char", [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (args[0].is_char()) {
      resolve_out(vm, argc, args, 1)->put(args[0].as_char());
    }
    return Value::unspecified();
  }, 1, 2));

  def_global("write", heap.make_subr("write", [](VM &vm, uint32_t argc, Value *args) -> Value {
    *resolve_out(vm, argc, args, 1) << vm.format_value(args[0]);
    return Value::unspecified();
  }, 1, 2));

  // %set-current-output-port! — internal-only (reserved-name convention,
  // see %bracket-vector/%brace-map), not exported as a general Scheme
  // mutator. Real parameter objects (make-parameter/parameterize) would
  // be the R7RS-shaped way to expose rebindable dynamic state generally;
  // absent those, this stays a narrow primitive that only the
  // with-output-to-file bootstrap below calls, itself wrapped in
  // unwind-protect for the restore-on-every-exit guarantee that used to
  // live in ScopedOutPortRebind/ScopedPortCloser (removed — cleanup is
  // now a language feature, not a C++ RAII pattern reinvented per call
  // site).
  def_global("%set-current-output-port!", heap.make_subr("%set-current-output-port!", [](VM &vm, uint32_t, Value *args) -> Value {
    if (Heap::is_port(args[0])) vm.current_out_port = args[0];
    return Value::unspecified();
  }, 1, 1));

  def_global("open-input-file", heap.make_subr("open-input-file", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0])) return Value::boolean_false();
    auto ifs = open_input_with_fallback(std::string(args[0].as_ptr<ObjString>()->view()));
    if (!ifs) return Value::boolean_false();
    return vm.heap.make_input_file_port(std::move(ifs));
  }, 1, 1));

  def_global("open-output-file", heap.make_subr("open-output-file", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0])) return Value::boolean_false();
    auto ofs = std::make_unique<std::ofstream>(std::string(args[0].as_ptr<ObjString>()->view()));
    if (!ofs->is_open()) return Value::boolean_false();
    return vm.heap.make_output_file_port(std::move(ofs));
  }, 1, 1));

  // with-output-to-file / call-with-input-file / call-with-output-file:
  // bootstrapped in Scheme (same pattern as force/assert above) rather
  // than as C++ subrs — unwind-protect now provides exactly the
  // restore-on-every-exit guarantee these needed, so the C++-RAII
  // version of that guarantee (ScopedOutPortRebind/ScopedPortCloser) is
  // gone; the language has the feature natively. open-*-file returning
  // #f on failure is unchanged, but the friendly with-*/call-with-*
  // wrappers now signal a clear error instead of silently returning
  // unspecified/#f — matching this session's running "no silent
  // failures" policy (see the for-each fix).
  {
    Reader r(*this,
      "(define (with-output-to-file filename thunk)"
      "  (let ((port (open-output-file filename)))"
      "    (if (not port) (error 'with-output-to-file \"could not open file:\" filename)"
      "        (let ((prev (current-output-port)))"
      "          (%set-current-output-port! port)"
      "          (unwind-protect (thunk)"
      "            (%set-current-output-port! prev)"
      "            (close-output-port port))))))"
      "(define (call-with-input-file filename proc)"
      "  (let ((port (open-input-file filename)))"
      "    (if (not port) (error 'call-with-input-file \"could not open file:\" filename)"
      "        (unwind-protect (proc port) (close-input-port port)))))"
      "(define (call-with-output-file filename proc)"
      "  (let ((port (open-output-file filename)))"
      "    (if (not port) (error 'call-with-output-file \"could not open file:\" filename)"
      "        (unwind-protect (proc port) (close-output-port port)))))");
    while (true) {
      Value form = r.read_form();
      if (form.is_eof()) break;
      Compiler comp(*this);
      ObjClosure *cl = comp.compile_top_level(form);
      call_closure(cl, {});
    }
  }

  def_global("read-char", heap.make_subr("read-char", [](VM &vm, uint32_t argc, Value *args) -> Value {
    std::istream *is = resolve_in(vm, argc, args, 0);
    int c = is->get();
    if (c == EOF) return Value::eof_obj();
    return Value::from_char(static_cast<char>(c));
  }, 0, 1));

  def_global("peek-char", heap.make_subr("peek-char", [](VM &vm, uint32_t argc, Value *args) -> Value {
    std::istream *is = resolve_in(vm, argc, args, 0);
    int c = is->peek();
    if (c == EOF) return Value::eof_obj();
    return Value::from_char(static_cast<char>(c));
  }, 0, 1));

  def_global("eof-object?", heap.make_subr("eof-object?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_eof());
  }, 1, 1));

  def_global("input-port?", heap.make_subr("input-port?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_port(args[0]) && args[0].as_ptr<ObjPort>()->is_input);
  }, 1, 1));

  def_global("output-port?", heap.make_subr("output-port?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_port(args[0]) && !args[0].as_ptr<ObjPort>()->is_input);
  }, 1, 1));

  def_global("current-input-port", heap.make_subr("current-input-port", [](VM &vm, uint32_t, Value *) -> Value {
    return vm.current_in_port;
  }, 0, 0));

  def_global("current-output-port", heap.make_subr("current-output-port", [](VM &vm, uint32_t, Value *) -> Value {
    return vm.current_out_port;
  }, 0, 0));

  def_global("close-input-port", heap.make_subr("close-input-port", [](VM &, uint32_t, Value *args) -> Value {
    if (Heap::is_port(args[0]) && args[0].as_ptr<ObjPort>()->is_input) {
      args[0].as_ptr<ObjPort>()->close_port();
    }
    return Value::unspecified();
  }, 1, 1));
  def_global("close-input-file", heap.make_subr("close-input-file", [](VM &, uint32_t, Value *args) -> Value {
    if (Heap::is_port(args[0]) && args[0].as_ptr<ObjPort>()->is_input) {
      args[0].as_ptr<ObjPort>()->close_port();
    }
    return Value::unspecified();
  }, 1, 1));
  def_global("close-output-port", heap.make_subr("close-output-port", [](VM &, uint32_t, Value *args) -> Value {
    if (Heap::is_port(args[0]) && !args[0].as_ptr<ObjPort>()->is_input) {
      args[0].as_ptr<ObjPort>()->close_port();
    }
    return Value::unspecified();
  }, 1, 1));

  // Property list table: (put sym prop val)
  def_global("put", heap.make_subr("put", [](VM &vm, uint32_t, Value *args) -> Value {
    std::string s1 = vm.format_value(args[0]);
    std::string s2 = vm.format_value(args[1]);
    vm.property_table[{s1, s2}] = args[2];
    return Value::boolean_true();
  }, 3, 3));

  // Bitwise arithmetic
  auto subr_logand = [](VM &, uint32_t argc, Value *args) -> Value {
    int64_t res = -1;
    for (uint32_t i = 0; i < argc; ++i) {
      int64_t v = args[i].is_int() ? args[i].as_int() : static_cast<int64_t>(args[i].as_real());
      res &= v;
    }
    return Value::from_int(static_cast<int32_t>(res));
  };
  def_global("logand", heap.make_subr("logand", subr_logand, 0, UINT32_MAX));
  def_global("bitwise-and", heap.make_subr("bitwise-and", subr_logand, 0, UINT32_MAX));

  auto subr_logior = [](VM &, uint32_t argc, Value *args) -> Value {
    int64_t res = 0;
    for (uint32_t i = 0; i < argc; ++i) {
      int64_t v = args[i].is_int() ? args[i].as_int() : static_cast<int64_t>(args[i].as_real());
      res |= v;
    }
    return Value::from_int(static_cast<int32_t>(res));
  };
  def_global("logior", heap.make_subr("logior", subr_logior, 0, UINT32_MAX));
  def_global("bitwise-ior", heap.make_subr("bitwise-ior", subr_logior, 0, UINT32_MAX));

  auto subr_logxor = [](VM &, uint32_t argc, Value *args) -> Value {
    int64_t res = 0;
    for (uint32_t i = 0; i < argc; ++i) {
      int64_t v = args[i].is_int() ? args[i].as_int() : static_cast<int64_t>(args[i].as_real());
      res ^= v;
    }
    return Value::from_int(static_cast<int32_t>(res));
  };
  def_global("logxor", heap.make_subr("logxor", subr_logxor, 0, UINT32_MAX));
  def_global("bitwise-xor", heap.make_subr("bitwise-xor", subr_logxor, 0, UINT32_MAX));

  auto subr_lognot = [](VM &, uint32_t, Value *args) -> Value {
    int64_t v = args[0].is_int() ? args[0].as_int() : static_cast<int64_t>(args[0].as_real());
    return Value::from_int(static_cast<int32_t>(~v));
  };
  def_global("lognot", heap.make_subr("lognot", subr_lognot, 1, 1));
  def_global("bitwise-not", heap.make_subr("bitwise-not", subr_lognot, 1, 1));

  auto subr_ash = [](VM &, uint32_t, Value *args) -> Value {
    int64_t n = args[0].is_int() ? args[0].as_int() : static_cast<int64_t>(args[0].as_real());
    int32_t count = args[1].is_int() ? args[1].as_int() : static_cast<int32_t>(args[1].as_real());
    int64_t res = count >= 0 ? (n << count) : (n >> (-count));
    return Value::from_int(static_cast<int32_t>(res));
  };
  def_global("ash", heap.make_subr("ash", subr_ash, 2, 2));
  def_global("arithmetic-shift", heap.make_subr("arithmetic-shift", subr_ash, 2, 2));

  // Equivalence predicate eqv?
  auto subr_eqv = [](VM &, uint32_t, Value *args) -> Value {
    Value a = args[0];
    Value b = args[1];
    if (a.raw == b.raw) return Value::boolean_true();
    if (a.is_int() && b.is_int()) return Value::from_bool(a.as_int() == b.as_int());
    if (a.is_double() && b.is_double()) return Value::from_bool(a.as_double() == b.as_double());
    if (a.is_char() && b.is_char()) return Value::from_bool(a.as_char() == b.as_char());
    return Value::boolean_false();
  };
  def_global("eqv?", heap.make_subr("eqv?", subr_eqv, 2, 2));

  // Concurrency
  auto subr_active_count = [](VM &vm, uint32_t, Value *) -> Value {
    return Value::from_int(static_cast<int32_t>(vm.active_fibers.size()));
  };
  def_global("active-fibers-count", heap.make_subr("active-fibers-count", subr_active_count, 0, 0));

  // (run-fibers [max-rounds]) — pump the scheduler until every active
  // fiber completes, or for at most max-rounds full rounds. Each round
  // steps each fiber to its own (yield)/completion/error — never an
  // arbitrary instruction boundary, which is the atomicity Scheme code
  // gets to rely on (the yield points are the lock boundaries; there are
  // no locks because none are needed). The optional argument counts
  // ROUNDS — a unit a Scheme program can reason about — not instructions,
  // which it never could. Unbounded by default: a fiber that never
  // yields is the program's bug, same as any other infinite loop.
  auto subr_run_fibers = [](VM &vm, uint32_t argc, Value *args) -> Value {
    size_t max_rounds = argc > 0 ? static_cast<size_t>(args[0].as_real()) : VM::UNBOUNDED;
    for (size_t round = 0; !vm.active_fibers.empty() && round < max_rounds; ++round) {
      vm.step_all_active_fibers();
    }
    return Value::from_int(static_cast<int32_t>(vm.active_fibers.size()));
  };
  def_global("run-fibers", heap.make_subr("run-fibers", subr_run_fibers, 0, 1));

  // ---------------------------------------------------------------------------
  // Modern Collections: Vectors
  // ---------------------------------------------------------------------------
  auto subr_vector = [](VM &vm, uint32_t argc, Value *args) -> Value {
    Value vec = vm.heap.make_vector(argc);
    ObjVector *ov = vec.as_ptr<ObjVector>();
    for (uint32_t i = 0; i < argc; ++i) ov->set(i, args[i]);
    return vec;
  };
  def_global("vector", heap.make_subr("vector", subr_vector, 0, UINT32_MAX));
  // Reserved name the reader desugars [...] to (vx_reader.h) — kept distinct
  // from "vector" itself so quote/quasiquote/do/let's bracket-form detection
  // (see quote_materialize etc. in vx_compiler.h) can't be confused by a
  // user's own quoted list that happens to start with the symbol `vector`.
  def_global("%bracket-vector", heap.make_subr("%bracket-vector", subr_vector, 0, UINT32_MAX));

  auto subr_make_vector = [](VM &vm, uint32_t argc, Value *args) -> Value {
    uint32_t size = static_cast<uint32_t>(args[0].as_int());
    Value fill = argc > 1 ? args[1] : Value::unspecified();
    return vm.heap.make_vector(size, fill);
  };
  def_global("make-vector", heap.make_subr("make-vector", subr_make_vector, 1, 2));

  auto subr_vector_ref = [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_vector(args[0])) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] vector-ref: expected vector, got " + vm.format_value(args[0]);
      }
      return Value::unspecified();
    }
    ObjVector *ov = args[0].as_ptr<ObjVector>();
    int32_t ix = args[1].as_int();
    if (ix < 0 || static_cast<uint32_t>(ix) >= ov->size) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] vector-ref: index " + std::to_string(ix) + " out of bounds";
      }
      return Value::unspecified();
    }
    return ov->get(static_cast<uint32_t>(ix));
  };
  def_global("vector-ref", heap.make_subr("vector-ref", subr_vector_ref, 2, 2));

  auto subr_vector_set = [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_vector(args[0])) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] vector-set!: expected vector, got " + vm.format_value(args[0]);
      }
      return Value::unspecified();
    }
    ObjVector *ov = args[0].as_ptr<ObjVector>();
    int32_t ix = args[1].as_int();
    if (ix < 0 || static_cast<uint32_t>(ix) >= ov->size) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] vector-set!: index " + std::to_string(ix) + " out of bounds";
      }
      return Value::unspecified();
    }
    ov->set(static_cast<uint32_t>(ix), args[2]);
    return Value::unspecified();
  };
  def_global("vector-set!", heap.make_subr("vector-set!", subr_vector_set, 3, 3));

  def_global("vector-length", heap.make_subr("vector-length", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_vector(args[0])) return Value::from_int(0);
    return Value::from_int(static_cast<int32_t>(args[0].as_ptr<ObjVector>()->size));
  }, 1, 1));

  def_global("list->vector", heap.make_subr("list->vector", [](VM &vm, uint32_t, Value *args) -> Value {
    std::vector<Value> elems;
    Value cur = args[0];
    while (Heap::is_cons(cur)) {
      elems.push_back(Heap::car(cur));
      cur = Heap::cdr(cur);
    }
    ObjVector *vec = vm.heap.allocate<ObjVector>(static_cast<uint32_t>(elems.size()));
    for (size_t i = 0; i < elems.size(); ++i) vec->set(i, elems[i]);
    return Value::from_ptr(vec);
  }, 1, 1));

  def_global("vector->list", heap.make_subr("vector->list", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_vector(args[0])) return Value::nil();
    ObjVector *vec = args[0].as_ptr<ObjVector>();
    Value res = Value::nil();
    vm.push_temp_root(&res);
    for (int i = static_cast<int>(vec->size) - 1; i >= 0; --i) {
      res = vm.heap.cons(vec->get(i), res);
    }
    vm.pop_temp_root();
    return res;
  }, 1, 1));

  def_global("string->list", heap.make_subr("string->list", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0])) return Value::nil();
    std::string_view sv = args[0].as_ptr<ObjString>()->view();
    Value res = Value::nil();
    vm.push_temp_root(&res);
    for (int i = static_cast<int>(sv.size()) - 1; i >= 0; --i) {
      res = vm.heap.cons(Value::from_char(sv[i]), res);
    }
    vm.pop_temp_root();
    return res;
  }, 1, 1));

  def_global("void", heap.make_subr("void", [](VM &, uint32_t, Value *) -> Value {
    return Value::unspecified();
  }, 0, UINT32_MAX));

  def_global("unspecified", heap.make_subr("unspecified", [](VM &, uint32_t, Value *) -> Value {
    return Value::unspecified();
  }, 0, 0));

  // ---------------------------------------------------------------------------
  // Modern Collections: Associative Maps
  // ---------------------------------------------------------------------------
  auto subr_hash_map = [](VM &vm, uint32_t argc, Value *args) -> Value {
    std::vector<std::pair<Value, Value>> kvs;
    for (uint32_t i = 0; i + 1 < argc; i += 2) {
      kvs.push_back({args[i], args[i + 1]});
    }
    return vm.heap.make_map(std::move(kvs));
  };
  def_global("hash-map", heap.make_subr("hash-map", subr_hash_map, 0, UINT32_MAX));
  // Reserved name the reader desugars {...} to — see the %bracket-vector
  // comment above; same reasoning, for maps.
  def_global("%brace-map", heap.make_subr("%brace-map", subr_hash_map, 0, UINT32_MAX));
  def_global("make-hash-map", heap.make_subr("make-hash-map", [](VM &vm, uint32_t, Value *) -> Value {
    return vm.heap.make_map({});
  }, 0, 0));

  auto subr_map_ref = [](VM &vm, uint32_t argc, Value *args) -> Value {
    if (!Heap::is_map(args[0])) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] map-ref: expected map, got " + vm.format_value(args[0]);
      }
      return Value::unspecified();
    }
    ObjMap *m = args[0].as_ptr<ObjMap>();
    Value def_val = argc > 2 ? args[2] : Value::nil();
    return m->get(args[1], def_val);
  };
  def_global("map-ref", heap.make_subr("map-ref", subr_map_ref, 2, 3));
  def_global("hash-map-ref", heap.make_subr("hash-map-ref", subr_map_ref, 2, 3));

  auto subr_map_set = [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_map(args[0])) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] map-set!: expected map, got " + vm.format_value(args[0]);
      }
      return Value::unspecified();
    }
    ObjMap *m = args[0].as_ptr<ObjMap>();
    m->set(args[1], args[2]);
    return args[0];
  };
  def_global("map-set!", heap.make_subr("map-set!", subr_map_set, 3, 3));
  def_global("hash-map-set!", heap.make_subr("hash-map-set!", subr_map_set, 3, 3));

  def_global("map-has?", heap.make_subr("map-has?", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_map(args[0])) return Value::boolean_false();
    return Value::from_bool(args[0].as_ptr<ObjMap>()->has(args[1]));
  }, 2, 2));
  def_global("hash-map-has?", heap.make_subr("hash-map-has?", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_map(args[0])) return Value::boolean_false();
    return Value::from_bool(args[0].as_ptr<ObjMap>()->has(args[1]));
  }, 2, 2));

  def_global("map-keys", heap.make_subr("map-keys", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_map(args[0])) return Value::nil();
    ObjMap *m = args[0].as_ptr<ObjMap>();
    Value res = Value::nil();
    vm.push_temp_root(&res);
    for (auto it = m->entries.rbegin(); it != m->entries.rend(); ++it) {
      res = vm.heap.cons(it->first, res);
    }
    vm.pop_temp_root();
    return res;
  }, 1, 1));

  def_global("map-values", heap.make_subr("map-values", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_map(args[0])) return Value::nil();
    ObjMap *m = args[0].as_ptr<ObjMap>();
    Value res = Value::nil();
    vm.push_temp_root(&res);
    for (auto it = m->entries.rbegin(); it != m->entries.rend(); ++it) {
      res = vm.heap.cons(it->second, res);
    }
    vm.pop_temp_root();
    return res;
  }, 1, 1));

  def_global("map-count", heap.make_subr("map-count", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_map(args[0])) return Value::from_int(0);
    return Value::from_int(static_cast<int32_t>(args[0].as_ptr<ObjMap>()->entries.size()));
  }, 1, 1));

  def_global("map?", heap.make_subr("map?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_map(args[0]));
  }, 1, 1));
  def_global("hash-map?", heap.make_subr("hash-map?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_map(args[0]));
  }, 1, 1));

  // ---------------------------------------------------------------------------
  // Polymorphic get
  // ---------------------------------------------------------------------------
  auto subr_get = [](VM &vm, uint32_t argc, Value *args) -> Value {
    Value coll = args[0];
    Value key = args[1];
    Value def_val = argc > 2 ? args[2] : Value::nil();
    if (Heap::is_map(coll)) {
      return coll.as_ptr<ObjMap>()->get(key, def_val);
    }
    if (Heap::is_vector(coll)) {
      ObjVector *ov = coll.as_ptr<ObjVector>();
      if (key.is_int()) {
        int32_t ix = key.as_int();
        if (ix >= 0 && static_cast<uint32_t>(ix) < ov->size) return ov->get(static_cast<uint32_t>(ix));
      }
      return def_val;
    }
    if (Heap::is_cons(coll)) {
      if (key.is_int()) {
        int32_t ix = key.as_int();
        Value cur = coll;
        while (ix > 0 && Heap::is_cons(cur)) {
          cur = Heap::cdr(cur);
          --ix;
        }
        if (ix == 0 && Heap::is_cons(cur)) return Heap::car(cur);
      }
      return def_val;
    }
    // Property list fallback: (get symbol prop [default])
    std::string s1 = vm.format_value(coll);
    std::string s2 = vm.format_value(key);
    auto it = vm.property_table.find({s1, s2});
    if (it != vm.property_table.end()) return it->second;
    return argc > 2 ? def_val : Value::boolean_false();
  };
  def_global("get", heap.make_subr("get", subr_get, 2, 3));

  // ---------------------------------------------------------------------------
  // Keywords
  // ---------------------------------------------------------------------------
  def_global("keyword?", heap.make_subr("keyword?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_keyword());
  }, 1, 1));

  def_global("symbol?", heap.make_subr("symbol?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].is_symbol());
  }, 1, 1));

  def_global("symbol->string", heap.make_subr("symbol->string", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!args[0].is_symbol()) return vm.heap.make_string("");
    return vm.heap.make_string(vm.get_symbol_name(args[0].as_symbol_id()));
  }, 1, 1));

  def_global("string->symbol", heap.make_subr("string->symbol", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0])) return Value::unspecified();
    std::string s(args[0].as_ptr<ObjString>()->view());
    return Value::from_symbol_id(vm.intern(s));
  }, 1, 1));

  def_global("keyword->string", heap.make_subr("keyword->string", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!args[0].is_keyword()) return vm.heap.make_string("");
    return vm.heap.make_string(vm.get_symbol_name(args[0].as_keyword_id()));
  }, 1, 1));

  def_global("string->keyword", heap.make_subr("string->keyword", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0])) return Value::unspecified();
    std::string_view sv = args[0].as_ptr<ObjString>()->view();
    uint32_t id = vm.intern(std::string(sv));
    return Value::from_keyword_id(id);
  }, 1, 1));

  // ---------------------------------------------------------------------------
  // Metaprogramming & Macros
  // ---------------------------------------------------------------------------
  def_global("gensym", heap.make_subr("gensym", [](VM &vm, uint32_t argc, Value *args) -> Value {
    std::string pfx = "g";
    if (argc > 0) {
      if (Heap::is_string(args[0])) pfx = std::string(args[0].as_ptr<ObjString>()->view());
      else if (args[0].is_symbol()) pfx = vm.get_symbol_name(args[0].as_symbol_id());
    }
    std::string sym = pfx + "_" + std::to_string(vm.next_gensym_id++);
    return Value::from_symbol_id(vm.intern(sym));
  }, 0, 1));

  auto subr_macroexpand_1 = [](VM &vm, uint32_t, Value *args) -> Value {
    Value form = args[0];
    if (Heap::is_cons(form) && Heap::car(form).is_symbol()) {
      std::string op = vm.get_symbol_name(Heap::car(form).as_symbol_id());
      auto it = vm.macros.find(op);
      if (it != vm.macros.end()) {
        std::vector<Value> raw_args;
        Value cur = Heap::cdr(form);
        while (Heap::is_cons(cur)) {
          raw_args.push_back(Heap::car(cur));
          cur = Heap::cdr(cur);
        }
        return vm.call_closure(it->second, raw_args);
      }
    }
    return form;
  };
  def_global("macroexpand-1", heap.make_subr("macroexpand-1", subr_macroexpand_1, 1, 1));

  auto subr_macroexpand = [](VM &vm, uint32_t, Value *args) -> Value {
    Value form = args[0];
    while (Heap::is_cons(form) && Heap::car(form).is_symbol()) {
      std::string op = vm.get_symbol_name(Heap::car(form).as_symbol_id());
      auto it = vm.macros.find(op);
      if (it != vm.macros.end()) {
        std::vector<Value> raw_args;
        Value cur = Heap::cdr(form);
        while (Heap::is_cons(cur)) {
          raw_args.push_back(Heap::car(cur));
          cur = Heap::cdr(cur);
        }
        form = vm.call_closure(it->second, raw_args);
      } else {
        break;
      }
    }
    return form;
  };
  def_global("macroexpand", heap.make_subr("macroexpand", subr_macroexpand, 1, 1));

  def_global("macro?", heap.make_subr("macro?", [](VM &vm, uint32_t, Value *args) -> Value {
    if (args[0].is_symbol()) {
      std::string name = vm.get_symbol_name(args[0].as_symbol_id());
      return Value::from_bool(vm.macros.find(name) != vm.macros.end());
    }
    return Value::boolean_false();
  }, 1, 1));

  // ---------------------------------------------------------------------------
  // Standard Scheme Predicates & Numeric Operations
  // ---------------------------------------------------------------------------
  def_global("zero?", heap.make_subr("zero?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].as_real() == 0.0);
  }, 1, 1));

  def_global("positive?", heap.make_subr("positive?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].as_real() > 0.0);
  }, 1, 1));

  def_global("negative?", heap.make_subr("negative?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].as_real() < 0.0);
  }, 1, 1));

  def_global("even?", heap.make_subr("even?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].as_int() % 2 == 0);
  }, 1, 1));

  def_global("odd?", heap.make_subr("odd?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(args[0].as_int() % 2 != 0);
  }, 1, 1));

  def_global("quotient", heap.make_subr("quotient", [](VM &, uint32_t, Value *args) -> Value {
    int64_t a = args[0].is_int() ? args[0].as_int() : static_cast<int64_t>(args[0].as_real());
    int64_t b = args[1].is_int() ? args[1].as_int() : static_cast<int64_t>(args[1].as_real());
    if (b == 0) return Value::from_int(0);
    int64_t q = a / b;
    if (q >= INT32_MIN && q <= INT32_MAX) return Value::from_int(static_cast<int32_t>(q));
    return Value::from_double(static_cast<double>(q));
  }, 2, 2));

  def_global("remainder", heap.make_subr("remainder", [](VM &, uint32_t, Value *args) -> Value {
    int64_t a = args[0].is_int() ? args[0].as_int() : static_cast<int64_t>(args[0].as_real());
    int64_t b = args[1].is_int() ? args[1].as_int() : static_cast<int64_t>(args[1].as_real());
    if (b == 0) return Value::from_int(0);
    int64_t r = a % b;
    return Value::from_int(static_cast<int32_t>(r));
  }, 2, 2));

  def_global("modulo", heap.make_subr("modulo", [](VM &, uint32_t, Value *args) -> Value {
    int64_t a = args[0].is_int() ? args[0].as_int() : static_cast<int64_t>(args[0].as_real());
    int64_t b = args[1].is_int() ? args[1].as_int() : static_cast<int64_t>(args[1].as_real());
    if (b == 0) return Value::from_int(0);
    int64_t r = a % b;
    if ((r > 0 && b < 0) || (r < 0 && b > 0)) r += b;
    return Value::from_int(static_cast<int32_t>(r));
  }, 2, 2));

  def_global("truncate", heap.make_subr("truncate", [](VM &, uint32_t, Value *args) -> Value {
    if (args[0].is_int()) return args[0];
    return Value::from_double(std::trunc(args[0].as_real()));
  }, 1, 1));

  // Structural Equality
  def_global("equal?", heap.make_subr("equal?", [](VM &, uint32_t, Value *args) -> Value {
    auto eq_helper = [](auto &self, Value a, Value b) -> bool {
      if (a.raw == b.raw) return true;
      if (a.is_int() && b.is_double()) return static_cast<double>(a.as_int()) == b.as_double();
      if (a.is_double() && b.is_int()) return a.as_double() == static_cast<double>(b.as_int());
      if (a.is_double() && b.is_double()) return a.as_double() == b.as_double();
      if (Heap::is_string(a) && Heap::is_string(b)) {
        return a.as_ptr<ObjString>()->view() == b.as_ptr<ObjString>()->view();
      }
      if (Heap::is_cons(a) && Heap::is_cons(b)) {
        return self(self, Heap::car(a), Heap::car(b)) && self(self, Heap::cdr(a), Heap::cdr(b));
      }
      if (Heap::is_vector(a) && Heap::is_vector(b)) {
        ObjVector *va = a.as_ptr<ObjVector>();
        ObjVector *vb = b.as_ptr<ObjVector>();
        if (va->size != vb->size) return false;
        for (uint32_t i = 0; i < va->size; ++i) {
          if (!self(self, va->get(i), vb->get(i))) return false;
        }
        return true;
      }
      return false;
    };
    return Value::from_bool(eq_helper(eq_helper, args[0], args[1]));
  }, 2, 2));

  def_global("memq", heap.make_subr("memq", [](VM &, uint32_t, Value *args) -> Value {
    Value item = args[0];
    Value cur = args[1];
    while (Heap::is_cons(cur)) {
      if (Heap::car(cur) == item) return cur;
      cur = Heap::cdr(cur);
    }
    return Value::boolean_false();
  }, 2, 2));

  def_global("assq", heap.make_subr("assq", [](VM &, uint32_t, Value *args) -> Value {
    Value key = args[0];
    Value cur = args[1];
    while (Heap::is_cons(cur)) {
      Value pair = Heap::car(cur);
      if (Heap::is_cons(pair) && Heap::car(pair) == key) return pair;
      cur = Heap::cdr(cur);
    }
    return Value::boolean_false();
  }, 2, 2));

  auto subr_memv = [](VM &, uint32_t, Value *args) -> Value {
    Value item = args[0];
    Value cur = args[1];
    while (Heap::is_cons(cur)) {
      Value elem = Heap::car(cur);
      bool match = (elem == item);
      if (!match && elem.is_int() && item.is_double()) match = static_cast<double>(elem.as_int()) == item.as_double();
      if (!match && elem.is_double() && item.is_int()) match = elem.as_double() == static_cast<double>(item.as_int());
      if (!match && elem.is_double() && item.is_double()) match = elem.as_double() == item.as_double();
      if (match) return cur;
      cur = Heap::cdr(cur);
    }
    return Value::boolean_false();
  };
  def_global("memv", heap.make_subr("memv", subr_memv, 2, 2));

  def_global("member", heap.make_subr("member", [](VM &, uint32_t, Value *args) -> Value {
    auto eq_helper = [](auto &self, Value a, Value b) -> bool {
      if (a.raw == b.raw) return true;
      if (a.is_int() && b.is_double()) return static_cast<double>(a.as_int()) == b.as_double();
      if (a.is_double() && b.is_int()) return a.as_double() == static_cast<double>(b.as_int());
      if (a.is_double() && b.is_double()) return a.as_double() == b.as_double();
      if (Heap::is_string(a) && Heap::is_string(b)) {
        return a.as_ptr<ObjString>()->view() == b.as_ptr<ObjString>()->view();
      }
      if (Heap::is_cons(a) && Heap::is_cons(b)) {
        return self(self, Heap::car(a), Heap::car(b)) && self(self, Heap::cdr(a), Heap::cdr(b));
      }
      if (Heap::is_vector(a) && Heap::is_vector(b)) {
        ObjVector *va = a.as_ptr<ObjVector>();
        ObjVector *vb = b.as_ptr<ObjVector>();
        if (va->size != vb->size) return false;
        for (uint32_t i = 0; i < va->size; ++i) {
          if (!self(self, va->get(i), vb->get(i))) return false;
        }
        return true;
      }
      return false;
    };
    Value item = args[0];
    Value cur = args[1];
    while (Heap::is_cons(cur)) {
      Value elem = Heap::car(cur);
      if (eq_helper(eq_helper, elem, item)) return cur;
      cur = Heap::cdr(cur);
    }
    return Value::boolean_false();
  }, 2, 2));

  auto subr_assv = [](VM &, uint32_t, Value *args) -> Value {
    Value key = args[0];
    Value cur = args[1];
    while (Heap::is_cons(cur)) {
      Value pair = Heap::car(cur);
      if (Heap::is_cons(pair)) {
        Value k = Heap::car(pair);
        bool match = (k == key);
        if (!match && k.is_int() && key.is_double()) match = static_cast<double>(k.as_int()) == key.as_double();
        if (!match && k.is_double() && key.is_int()) match = k.as_double() == static_cast<double>(key.as_int());
        if (!match && k.is_double() && key.is_double()) match = k.as_double() == key.as_double();
        if (match) return pair;
      }
      cur = Heap::cdr(cur);
    }
    return Value::boolean_false();
  };
  def_global("assv", heap.make_subr("assv", subr_assv, 2, 2));

  def_global("assoc", heap.make_subr("assoc", [](VM &, uint32_t, Value *args) -> Value {
    auto eq_helper = [](auto &self, Value a, Value b) -> bool {
      if (a.raw == b.raw) return true;
      if (a.is_int() && b.is_double()) return static_cast<double>(a.as_int()) == b.as_double();
      if (a.is_double() && b.is_int()) return a.as_double() == static_cast<double>(b.as_int());
      if (a.is_double() && b.is_double()) return a.as_double() == b.as_double();
      if (Heap::is_string(a) && Heap::is_string(b)) {
        return a.as_ptr<ObjString>()->view() == b.as_ptr<ObjString>()->view();
      }
      if (Heap::is_cons(a) && Heap::is_cons(b)) {
        return self(self, Heap::car(a), Heap::car(b)) && self(self, Heap::cdr(a), Heap::cdr(b));
      }
      if (Heap::is_vector(a) && Heap::is_vector(b)) {
        ObjVector *va = a.as_ptr<ObjVector>();
        ObjVector *vb = b.as_ptr<ObjVector>();
        if (va->size != vb->size) return false;
        for (uint32_t i = 0; i < va->size; ++i) {
          if (!self(self, va->get(i), vb->get(i))) return false;
        }
        return true;
      }
      return false;
    };
    Value key = args[0];
    Value cur = args[1];
    while (Heap::is_cons(cur)) {
      Value pair = Heap::car(cur);
      if (Heap::is_cons(pair)) {
        Value k = Heap::car(pair);
        if (eq_helper(eq_helper, k, key)) return pair;
      }
      cur = Heap::cdr(cur);
    }
    return Value::boolean_false();
  }, 2, 2));

  // Strings
  def_global("number->string", heap.make_subr("number->string", [](VM &vm, uint32_t argc, Value *args) -> Value {
    int radix = 10;
    if (argc > 1 && args[1].is_int()) radix = args[1].as_int();
    if (args[0].is_int() && radix != 10 && radix >= 2 && radix <= 36) {
      int32_t iv = args[0].as_int();
      bool neg = iv < 0;
      uint32_t uv = neg ? static_cast<uint32_t>(-static_cast<int64_t>(iv))
                        : static_cast<uint32_t>(iv);
      static const char DIGITS[] = "0123456789abcdefghijklmnopqrstuvwxyz";
      std::string digits;
      if (uv == 0) {
        digits = "0";
      } else {
        while (uv > 0) {
          digits.push_back(DIGITS[uv % static_cast<uint32_t>(radix)]);
          uv /= static_cast<uint32_t>(radix);
        }
        std::reverse(digits.begin(), digits.end());
      }
      if (neg) digits.insert(digits.begin(), '-');
      return vm.heap.make_string(digits);
    }
    if (args[0].is_int()) {
      return vm.heap.make_string(std::to_string(args[0].as_int()));
    }
    if (args[0].is_double()) {
      // Route through the one shared formatter (format_double, used by
      // format_value/write/display too) rather than a second copy of
      // this logic — a second copy is exactly how this bug happened:
      // whole-number doubles here used to short-circuit to
      // std::to_string(int64_t), producing bare integer text ("0" for
      // 0.0) with no decimal point at all, indistinguishable from the
      // exact integer 0. string->number then read it back as exact,
      // and (eqv? 0.0 0) is false (eqv? requires matching exactness) —
      // number->string must produce something string->number reads
      // back as the SAME exactness, not just the same magnitude.
      // format_double already guards this (appends ".0" whenever
      // to_chars' output has neither '.' nor 'e'); this path just
      // wasn't using it.
      return vm.heap.make_string(format_double(args[0].as_double()));
    }
    return vm.heap.make_string("0");
  }, 1, 2));

  def_global("string->number", heap.make_subr("string->number", [](VM &, uint32_t argc, Value *args) -> Value {
    if (!Heap::is_string(args[0])) return Value::boolean_false();
    std::string_view sv = args[0].as_ptr<ObjString>()->view();
    if (sv.empty() || sv == "+" || sv == "-" || sv == ".") return Value::boolean_false();

    int radix = 10;
    if (argc > 1) {
      if (!args[1].is_int()) return Value::boolean_false();
      radix = args[1].as_int();
      if (radix < 2 || radix > 36) return Value::boolean_false();
    }

    if (radix != 10) {
      // R4RS only requires exact (integer) parsing outside radix 10 — no
      // double fallback, so a magnitude that doesn't fit our 32-bit exact
      // integers is just a parse failure here (unlike the radix-10 case
      // below, which can fall back to inexact).
      char *end = nullptr;
      errno = 0;
      long long iv = std::strtoll(sv.data(), &end, radix);
      if (end != sv.data() + sv.size() || errno == ERANGE ||
          iv < INT32_MIN || iv > INT32_MAX) {
        return Value::boolean_false();
      }
      return Value::from_int(static_cast<int32_t>(iv));
    }

    char *end = nullptr;
    errno = 0;
    long long iv = std::strtoll(sv.data(), &end, 10);
    bool int_ok = end == sv.data() + sv.size() && errno != ERANGE &&
                  iv >= INT32_MIN && iv <= INT32_MAX;
    if (int_ok) return Value::from_int(static_cast<int32_t>(iv));
    end = nullptr;
    double dv = std::strtod(sv.data(), &end);
    if (end == sv.data() + sv.size()) return Value::from_double(dv);
    return Value::boolean_false();
  }, 1, 2));

  def_global("string-length", heap.make_subr("string-length", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0])) return Value::from_int(0);
    return Value::from_int(static_cast<int32_t>(args[0].as_ptr<ObjString>()->view().size()));
  }, 1, 1));

  def_global("string-ref", heap.make_subr("string-ref", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0])) return Value::from_char('\0');
    std::string_view sv = args[0].as_ptr<ObjString>()->view();
    int32_t ix = args[1].as_int();
    if (ix < 0 || static_cast<size_t>(ix) >= sv.size()) return Value::from_char('\0');
    return Value::from_char(sv[ix]);
  }, 2, 2));

  def_global("string-append", heap.make_subr("string-append", [](VM &vm, uint32_t argc, Value *args) -> Value {
    std::string res;
    for (uint32_t i = 0; i < argc; ++i) {
      if (Heap::is_string(args[i])) {
        res += args[i].as_ptr<ObjString>()->view();
      } else if (args[i].is_symbol()) {
        res += vm.get_symbol_name(args[i].as_symbol_id());
      }
    }
    return vm.heap.make_string(res);
  }, 0, UINT32_MAX));

  def_global("substring", heap.make_subr("substring", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0])) return vm.heap.make_string("");
    std::string_view sv = args[0].as_ptr<ObjString>()->view();
    int32_t start = std::max(0, args[1].as_int());
    int32_t end = std::min<int32_t>(static_cast<int32_t>(sv.size()), args[2].as_int());
    if (start >= end) return vm.heap.make_string("");
    return vm.heap.make_string(std::string(sv.substr(start, end - start)));
  }, 3, 3));

  def_global("make-string", heap.make_subr("make-string", [](VM &vm, uint32_t argc, Value *args) -> Value {
    uint32_t len = static_cast<uint32_t>(args[0].as_int());
    char fill = (argc > 1 && args[1].is_char()) ? args[1].as_char() : ' ';
    return vm.heap.make_string(std::string(len, fill));
  }, 1, 2));

  def_global("string", heap.make_subr("string", [](VM &vm, uint32_t argc, Value *args) -> Value {
    std::string s;
    s.reserve(argc);
    for (uint32_t i = 0; i < argc; ++i) {
      if (args[i].is_char()) s += args[i].as_char();
    }
    return vm.heap.make_string(s);
  }, 0, UINT32_MAX));

  def_global("string-copy", heap.make_subr("string-copy", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0])) return vm.heap.make_string("");
    return vm.heap.make_string(std::string(args[0].as_ptr<ObjString>()->view()));
  }, 1, 1));

  def_global("string-set!", heap.make_subr("string-set!", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0])) return Value::unspecified();
    ObjString *os = args[0].as_ptr<ObjString>();
    int32_t ix = args[1].as_int();
    if (ix >= 0 && static_cast<uint32_t>(ix) < os->length && args[2].is_char()) {
      os->chars[ix] = args[2].as_char();
    }
    return Value::unspecified();
  }, 3, 3));

  def_global("string-fill!", heap.make_subr("string-fill!", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0]) || !args[1].is_char()) return Value::unspecified();
    ObjString *os = args[0].as_ptr<ObjString>();
    std::memset(os->chars, args[1].as_char(), os->length);
    return Value::unspecified();
  }, 2, 2));

  def_global("vector-fill!", heap.make_subr("vector-fill!", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_vector(args[0])) return Value::unspecified();
    ObjVector *ov = args[0].as_ptr<ObjVector>();
    for (uint32_t i = 0; i < ov->size; ++i) ov->set(i, args[1]);
    return Value::unspecified();
  }, 2, 2));

  // R7RS's answer to "why won't for-each/map walk a vector": separate,
  // type-specific procedures rather than generalizing for-each/map
  // themselves (see yesterday's for-each fix — vx-scheme follows
  // mainstream Scheme here, not Clojure's single-seq-abstraction model).
  auto subr_vector_for_each = [](VM &vm, uint32_t argc, Value *args) -> Value {
    Value fn = args[0];
    for (uint32_t i = 1; i < argc; ++i) {
      if (!Heap::is_vector(args[i])) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] vector-for-each: not a vector, got " + vm.format_value(args[i]);
        return Value::unspecified();
      }
    }
    uint32_t len = args[1].as_ptr<ObjVector>()->size;
    for (uint32_t i = 2; i < argc; ++i) len = std::min(len, args[i].as_ptr<ObjVector>()->size);
    std::vector<Value> step_args(argc - 1);
    for (uint32_t i = 0; i < len; ++i) {
      for (uint32_t k = 1; k < argc; ++k) step_args[k - 1] = args[k].as_ptr<ObjVector>()->get(i);
      if (Heap::is_subr(fn)) {
        vm.call_subr(fn.as_ptr<ObjSubr>(), static_cast<uint32_t>(step_args.size()), step_args.data());
      } else if (Heap::is_closure(fn)) {
        vm.call_closure(fn.as_ptr<ObjClosure>(), step_args);
      }
    }
    return Value::unspecified();
  };
  def_global("vector-for-each", heap.make_subr("vector-for-each", subr_vector_for_each, 2, UINT32_MAX));

  auto subr_vector_map = [](VM &vm, uint32_t argc, Value *args) -> Value {
    Value fn = args[0];
    for (uint32_t i = 1; i < argc; ++i) {
      if (!Heap::is_vector(args[i])) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] vector-map: not a vector, got " + vm.format_value(args[i]);
        return Value::unspecified();
      }
    }
    uint32_t len = args[1].as_ptr<ObjVector>()->size;
    for (uint32_t i = 2; i < argc; ++i) len = std::min(len, args[i].as_ptr<ObjVector>()->size);
    // `out`'s underlying ObjVector, once rooted, has a stable address for
    // the rest of this call (the collector never moves live objects) —
    // safe to cache `ov` across the loop the same way map/filter's
    // result-chain accumulators are protected via push_temp_root.
    Value out = vm.heap.make_vector(len);
    vm.push_temp_root(&out);
    ObjVector *ov = out.as_ptr<ObjVector>();
    std::vector<Value> step_args(argc - 1);
    for (uint32_t i = 0; i < len; ++i) {
      for (uint32_t k = 1; k < argc; ++k) step_args[k - 1] = args[k].as_ptr<ObjVector>()->get(i);
      Value res = Value::unspecified();
      if (Heap::is_subr(fn)) {
        res = vm.call_subr(fn.as_ptr<ObjSubr>(), static_cast<uint32_t>(step_args.size()), step_args.data());
      } else if (Heap::is_closure(fn)) {
        res = vm.call_closure(fn.as_ptr<ObjClosure>(), step_args);
      }
      ov->set(i, res);
    }
    vm.pop_temp_root();
    return out;
  };
  def_global("vector-map", heap.make_subr("vector-map", subr_vector_map, 2, UINT32_MAX));

  auto subr_string_for_each = [](VM &vm, uint32_t argc, Value *args) -> Value {
    Value fn = args[0];
    for (uint32_t i = 1; i < argc; ++i) {
      if (!Heap::is_string(args[i])) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] string-for-each: not a string, got " + vm.format_value(args[i]);
        return Value::unspecified();
      }
    }
    uint32_t len = args[1].as_ptr<ObjString>()->length;
    for (uint32_t i = 2; i < argc; ++i) len = std::min(len, args[i].as_ptr<ObjString>()->length);
    std::vector<Value> step_args(argc - 1);
    for (uint32_t i = 0; i < len; ++i) {
      for (uint32_t k = 1; k < argc; ++k) step_args[k - 1] = Value::from_char(args[k].as_ptr<ObjString>()->chars[i]);
      if (Heap::is_subr(fn)) {
        vm.call_subr(fn.as_ptr<ObjSubr>(), static_cast<uint32_t>(step_args.size()), step_args.data());
      } else if (Heap::is_closure(fn)) {
        vm.call_closure(fn.as_ptr<ObjClosure>(), step_args);
      }
    }
    return Value::unspecified();
  };
  def_global("string-for-each", heap.make_subr("string-for-each", subr_string_for_each, 2, UINT32_MAX));

  auto subr_string_map = [](VM &vm, uint32_t argc, Value *args) -> Value {
    Value fn = args[0];
    for (uint32_t i = 1; i < argc; ++i) {
      if (!Heap::is_string(args[i])) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] string-map: not a string, got " + vm.format_value(args[i]);
        return Value::unspecified();
      }
    }
    uint32_t len = args[1].as_ptr<ObjString>()->length;
    for (uint32_t i = 2; i < argc; ++i) len = std::min(len, args[i].as_ptr<ObjString>()->length);
    // No GC-safety concern building this up as a plain std::string —
    // unlike vector-map's output, it isn't Scheme-heap-visible (and thus
    // isn't at risk from an intervening collection) until make_string
    // constructs the real ObjString at the very end.
    std::string result;
    result.reserve(len);
    std::vector<Value> step_args(argc - 1);
    for (uint32_t i = 0; i < len; ++i) {
      for (uint32_t k = 1; k < argc; ++k) step_args[k - 1] = Value::from_char(args[k].as_ptr<ObjString>()->chars[i]);
      Value res = Value::unspecified();
      if (Heap::is_subr(fn)) {
        res = vm.call_subr(fn.as_ptr<ObjSubr>(), static_cast<uint32_t>(step_args.size()), step_args.data());
      } else if (Heap::is_closure(fn)) {
        res = vm.call_closure(fn.as_ptr<ObjClosure>(), step_args);
      }
      if (res.is_char()) result += res.as_char();
    }
    return vm.heap.make_string(result);
  };
  def_global("string-map", heap.make_subr("string-map", subr_string_map, 2, UINT32_MAX));

  def_global("list->string", heap.make_subr("list->string", [](VM &vm, uint32_t, Value *args) -> Value {
    std::string s;
    Value cur = args[0];
    while (Heap::is_cons(cur)) {
      Value c = Heap::car(cur);
      if (c.is_char()) s += c.as_char();
      cur = Heap::cdr(cur);
    }
    return vm.heap.make_string(s);
  }, 1, 1));

  // Control & IO
  def_global("gc", heap.make_subr("gc", [](VM &vm, uint32_t, Value *) -> Value {
    vm.collect_garbage();
    return Value::unspecified();
  }, 0, 0));

  auto subr_procedure_p = [](VM &, uint32_t, Value *args) -> Value {
    Value v = args[0];
    return Value::from_bool(Heap::is_closure(v) || Heap::is_subr(v));
  };
  def_global("procedure?", heap.make_subr("procedure?", subr_procedure_p, 1, 1));

  // Initialize force as a pure Scheme procedure to avoid C++ recursion during deep lazy streams
  {
    Reader r(*this, "(define (force p) (if (procedure? p) (p) p))");
    Value form = r.read_form();
    Compiler comp(*this);
    ObjClosure *cl = comp.compile_top_level(form);
    call_closure(cl, {});
  }

  // assert as a defmacro (not a subr) so the failing *expression* — not
  // just its runtime-false value — shows up in the error, the way most
  // Lisps' assert does. Not an R7RS procedure (R7RS doesn't standardize
  // one at all), but a common enough extension across Schemes that it's
  // worth having in the "things people reflexively reach for" sense.
  {
    Reader r(*this, "(defmacro (assert expr) `(if (not ,expr) (error 'assert \"assertion failed:\" ',expr)))");
    Value form = r.read_form();
    Compiler comp(*this);
    ObjClosure *cl = comp.compile_top_level(form);
    call_closure(cl, {});
  }

  def_global("time", heap.make_subr("time", [](VM &vm, uint32_t, Value *args) -> Value {
    Value thunk = args[0];
    auto t0 = std::chrono::high_resolution_clock::now();
    Value res = Value::unspecified();
    vm.push_temp_root(&res);
    if (Heap::is_closure(thunk)) {
      res = vm.call_closure(thunk.as_ptr<ObjClosure>(), {});
    } else if (Heap::is_subr(thunk)) {
      res = vm.call_subr(thunk.as_ptr<ObjSubr>(), 0, nullptr);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    Value pair = vm.heap.cons(Value::from_double(secs), res);
    vm.pop_temp_root();
    return pair;
  }, 1, 1));

  def_global("load", heap.make_subr("load", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0])) return Value::unspecified();
    std::string filename = std::string(args[0].as_ptr<ObjString>()->view());
    std::ifstream file(filename);
    if (!file.is_open()) file.open("testcases/" + filename);
    if (!file.is_open()) file.open("../testcases/" + filename);
    if (!file.is_open()) file.open("../" + filename);
    if (!file.is_open()) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] load: cannot open file " + filename;
      }
      return Value::unspecified();
    }
    std::stringstream buf;
    buf << file.rdbuf();
    std::string code = buf.str();
    Reader reader(vm, code);
    Value last_res = Value::unspecified();
    while (true) {
      Value form = reader.read_form();
      if (form.is_eof()) break;
      Compiler compiler(vm);
      ObjClosure *closure = compiler.compile_top_level(form);
      last_res = vm.call_closure(closure, {});
      if (vm.current_fiber && vm.current_fiber->state == Fiber::State::Error) {
        return Value::unspecified();
      }
    }
    return last_res;
  }, 1, 1));

  // error now RAISES (an error-object) rather than directly killing the
  // fiber — the same mechanism (raise) that (guard ...) catches, so
  // every existing (error ...) call site in the codebase (assert, the
  // AOT/reader bootstrap snippets, ...) is transparently catchable now.
  // An uncaught error surfaces exactly as before: format_raised_value
  // produces the identical "[Scheme Error] reason irritant..." text,
  // and RaiseEscape derives from std::exception, so main.cpp's top-level
  // catch (and vx_wasm.cpp's) need no changes at all.
  def_global("error", heap.make_subr("error", [](VM &vm, uint32_t argc, Value *args) -> Value {
    std::vector<Value> irritants(args + 1, args + argc);
    Value err_obj = vm.heap.make_error_object(args[0], irritants);
    std::string msg = format_raised_value(vm, err_obj);
    vm.in_flight_raises.push_back(err_obj);
    throw RaiseEscape(msg);
  }, 1, UINT32_MAX));

  // (raise obj) — obj can be anything, not just an error-object (R7RS
  // permits (raise 'some-symbol), (raise 42), ...); error-object? is
  // what lets a guard clause tell the two apart. raise-continuable is
  // deliberately not implemented: it requires the handler's return value
  // to become raise-continuable's own return value, which is a
  // fundamentally different (non-escape) control shape than everything
  // else in this VM's exception handling — a real omission, not an
  // oversight, and one we're comfortable with per "no interest in Rx
  // compatibility for its own sake."
  def_global("raise", heap.make_subr("raise", [](VM &vm, uint32_t, Value *args) -> Value {
    std::string msg = format_raised_value(vm, args[0]);
    vm.in_flight_raises.push_back(args[0]);
    throw RaiseEscape(msg);
  }, 1, 1));

  def_global("error-object?", heap.make_subr("error-object?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_error_object(args[0]));
  }, 1, 1));

  def_global("error-object-message", heap.make_subr("error-object-message", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_error_object(args[0])) return Value::boolean_false();
    return args[0].as_ptr<ObjVector>()->get(0);
  }, 1, 1));

  def_global("error-object-irritants", heap.make_subr("error-object-irritants", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_error_object(args[0])) return Value::nil();
    ObjVector *ov = args[0].as_ptr<ObjVector>();
    Value res = Value::nil();
    vm.push_temp_root(&res);
    for (uint32_t i = ov->size; i > 1; --i) res = vm.heap.cons(ov->get(i - 1), res);
    vm.pop_temp_root();
    return res;
  }, 1, 1));

  def_global("scheme-implementation-type", heap.make_subr("scheme-implementation-type", [](VM &vm, uint32_t, Value *) -> Value {
    return Value::from_symbol_id(vm.intern("vx-scheme"));
  }, 0, 0));

  def_global("exit", heap.make_subr("exit", [](VM &, uint32_t argc, Value *args) -> Value {
    int code = (argc > 0 && args[0].is_int()) ? args[0].as_int() : 0;
    std::exit(code);
  }, 0, 1));

  def_global("scheme-implementation-platform", heap.make_subr("scheme-implementation-platform", [](VM &vm, uint32_t, Value *) -> Value {
    return Value::from_symbol_id(vm.intern("native"));
  }, 0, 0));

  def_global("vx-scheme-implementation-type", heap.make_subr("vx-scheme-implementation-type", [](VM &vm, uint32_t, Value *) -> Value {
    return Value::from_symbol_id(vm.intern("vm"));
  }, 0, 0));

  def_global("defined?", heap.make_subr("defined?", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!args[0].is_symbol() && !Heap::is_string(args[0])) return Value::boolean_false();
    std::string sym = args[0].is_symbol() ? vm.get_symbol_name(args[0].as_symbol_id()) : std::string(args[0].as_ptr<ObjString>()->view());
    return Value::from_bool(vm.globals.find(sym) != vm.globals.end());
  }, 1, 1));

  def_global("procedure?", heap.make_subr("procedure?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_closure(args[0]) || Heap::is_subr(args[0]) || args[0].is_keyword() || Heap::is_map(args[0]) || Heap::is_vector(args[0]));
  }, 1, 1));

  def_global("force", heap.make_subr("force", [](VM &vm, uint32_t, Value *args) -> Value {
    if (Heap::is_closure(args[0])) {
      return vm.call_closure(args[0].as_ptr<ObjClosure>(), {});
    }
    return args[0];
  }, 1, 1));

  def_global("promise?", heap.make_subr("promise?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_closure(args[0]));
  }, 1, 1));

  // Escape-only ("bounded") continuations. General re-entrant call/cc is
  // deliberately not supported (see vx_vm.h's ContinuationEscape comment
  // and ARCHITECTURE_ARC.md, Milestone 7) — the escape procedure below
  // may only be invoked to unwind outward, and only within the dynamic
  // extent of this call; invoking it later throws all the way to the
  // top level as an ordinary runtime error instead of resuming anything.
  auto subr_call_cc = [](VM &vm, uint32_t, Value *args) -> Value {
    Value proc = args[0];
    if (!Heap::is_closure(proc) && !Heap::is_subr(proc)) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message = "[VM Error] call/cc: not a procedure";
      }
      return Value::unspecified();
    }

    // A fresh ObjSubr per invocation; its own address is its identity
    // (see ContinuationEscape / VM::current_subr in vx_vm.h) — no
    // separate token bookkeeping needed.
    ObjSubr *escape = vm.heap.allocate<ObjSubr>(
        "continuation",
        [](VM &vm, uint32_t argc, Value *args) -> Value {
          throw ContinuationEscape(vm.current_subr, argc > 0 ? args[0] : Value::unspecified());
        },
        0, 1);
    Value escape_val = Value::from_ptr(escape);

    // A C++ exception unwinds the Scheme-level f.stack/f.frames' backing
    // vectors/deques (see call_closure's use of run_dispatch throughout
    // this file, and OP_TAIL_CALL and OP_CALL). C++ exception unwinding
    // only touches the *C++* call stack, not those — restore them
    // explicitly on the caught path so the OP_CALL that invoked this
    // very subr sees the same stack shape it would have on an ordinary
    // (non-escaping) return.
    Fiber *f = vm.current_fiber;
    size_t saved_stack_size = f ? f->stack.size() : 0;
    size_t saved_frames_size = f ? f->frames.size() : 0;
    size_t saved_winders_size = f ? f->winders.size() : 0;

    try {
      if (Heap::is_closure(proc)) {
        return vm.call_closure(proc.as_ptr<ObjClosure>(), {escape_val});
      }
      return vm.call_subr(proc.as_ptr<ObjSubr>(), 1, &escape_val);
    } catch (ContinuationEscape &e) {
      if (e.target != static_cast<Obj *>(escape)) throw; // not ours — keep unwinding
      // Copy out of the caught exception object before touching anything
      // that can allocate: e.value lives in C++ exception-handling
      // storage, which mark_roots has no way to see. That was harmless
      // right up until run_pending_winders below started existing —
      // a winder's cleanup can call_closure, which can GC, and at that
      // point e.value (if it's a fresh, otherwise-unrooted heap object)
      // would be invisible to the collector. escape_value is an ordinary
      // local instead, protected the ordinary way.
      Value escape_value = e.value;
      vm.push_temp_root(&escape_value);
      if (f) {
        f->stack.resize(saved_stack_size);
        f->frames.resize(saved_frames_size);
        // The frames just discarded may have entered unwind-protect
        // extents whose cleanups are still pending on the winder list —
        // run them (innermost first) now that the fiber is coherent
        // again, before handing the escape value back. This is the
        // "finally fires on the way out" half of the design.
        vm.run_pending_winders(*f, saved_winders_size);
      }
      vm.pop_temp_root();
      return escape_value;
    }
  };
  def_global("call-with-current-continuation", heap.make_subr("call-with-current-continuation", subr_call_cc, 1, 1));
  def_global("call/cc", heap.make_subr("call/cc", subr_call_cc, 1, 1));

  // %guard — internal-only (reserved-name convention), the C++ half of
  // the `guard` compiler special form below. Not itself compiled inline
  // the way unwind-protect is: unlike a pending cleanup (a value on a
  // list, consulted by whichever catch site gets there first), guard
  // needs an actual C++ try/catch scoping its body, and try/catch is
  // inherently tied to C++ call-stack depth — there is no bytecode-level
  // trick that gives a specific instruction pointer its own catch
  // boundary without a nested C++ call. That means (yield) inside a
  // guard body is illegal, exactly like inside a call/cc thunk today —
  // not a new limitation, the same one, for the same underlying reason.
  auto subr_guard = [](VM &vm, uint32_t, Value *args) -> Value {
    Value handler = args[0]; // (lambda (var) (cond clause... (else (raise var))))
    Value thunk = args[1];   // (lambda () body...)

    Fiber *f = vm.current_fiber;
    size_t saved_stack_size = f ? f->stack.size() : 0;
    size_t saved_frames_size = f ? f->frames.size() : 0;
    size_t saved_winders_size = f ? f->winders.size() : 0;

    try {
      if (Heap::is_closure(thunk)) {
        return vm.call_closure(thunk.as_ptr<ObjClosure>(), {});
      }
      return vm.call_subr(thunk.as_ptr<ObjSubr>(), 0, nullptr);
    } catch (RaiseEscape &) {
      // Whichever raise got us here pushed exactly one value; it's ours
      // to pop (see VM::in_flight_raises' comment on LIFO nesting).
      Value raised = vm.in_flight_raises.back();
      vm.in_flight_raises.pop_back();
      vm.push_temp_root(&raised);
      if (f) {
        f->stack.resize(saved_stack_size);
        f->frames.resize(saved_frames_size);
        vm.run_pending_winders(*f, saved_winders_size);
      }
      Value result;
      if (Heap::is_closure(handler)) {
        result = vm.call_closure(handler.as_ptr<ObjClosure>(), {raised});
      } else {
        result = vm.call_subr(handler.as_ptr<ObjSubr>(), 1, &raised);
      }
      vm.pop_temp_root();
      return result;
    }
  };
  def_global("%guard", heap.make_subr("%guard", subr_guard, 2, 2));

  // Sweep any temporary artifacts from bootstrap evaluation
  collect_garbage();
}

} // namespace vxs
