#include "vx_vm.h"
#include "vx_reader.h"
#include "vx_compiler.h"
#include "vx_embedded_libs.h"
#include <algorithm>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <climits>
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
      case ObjType::Handle: {
        ObjHandle *h = obj->as<ObjHandle>();
        return std::string("#<") + get_symbol_name(h->kind) + " " +
               std::to_string(h->id) + (h->released ? " (released)" : "") + ">";
      }
      case ObjType::Bytes: {
        ObjBytes *b = obj->as<ObjBytes>();
        return "#<bytes " + std::to_string(b->data.size()) + " " +
               (b->residency == ObjBytes::Residency::Building ? "building" : "sealed") + ">";
      }
      case ObjType::View: {
        ObjView *vw = obj->as<ObjView>();
        const char *n = "u8";
        switch (vw->elem) {
          case ElemType::U8:  n = "u8";  break;
          case ElemType::I32: n = "i32"; break;
          case ElemType::U32: n = "u32"; break;
          case ElemType::F32: n = "f32"; break;
          case ElemType::F64: n = "f64"; break;
        }
        return std::string("#<view ") + n + " x" + std::to_string(vw->count) + ">";
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
  mark_value(f->backing_future);
  mark_value(f->awaited);
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
    case ObjType::View: {
      // A view holds its buffer alive. Nothing else may: `(bytes-view b ...)`
      // is routinely the only surviving reference once b goes out of scope.
      mark_value(obj->as<ObjView>()->bytes);
      break;
    }
    case ObjType::String:
    case ObjType::Symbol:
    case ObjType::Subr:
    case ObjType::Fiber:
    case ObjType::Port:
    case ObjType::Handle:
    case ObjType::Bytes:
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
  // In-flight external futures. Nothing else may be holding these: the
  // program can drop a future before anyone touches it, and the callback
  // that will settle it lives outside the VM entirely.
  for (const auto &kv : pending_externals) {
    h.mark_value(kv.second);
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

  // 2. Sweep phase (sweep() already counts what it reclaimed; keep it
  // rather than discarding, so "did that collection actually do work?"
  // is answerable without a profiler.)
  last_gc_freed = sweep();
  total_objects_freed += last_gc_freed;
  ++gc_count;

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

      case OP_INIT_LOCAL: {
        uint16_t slot = read_u16(ip);
        // Unconditional raw store — the one thing OP_SET_LOCAL must not do.
        // Entering a scope creates a NEW binding, so if a previous entry's
        // binding was captured (OP_CLOSURE boxes the slot in place), this
        // must overwrite the slot rather than assign through that box. The
        // old box stays owned by the closure that captured it, which is
        // exactly the fresh-binding-per-entry semantics `let` requires.
        f.stack[frame->stack_base + slot] = f.pop();
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
        // Reverse link, so the scheduler can settle this future when the
        // child finishes. The child is already in active_fibers, so it is
        // rooted across the allocation above.
        child->backing_future = fut_val;
        f.push(fut_val);
        break;
      }

      case OP_TOUCH:
      case OP_TOUCH_VALUE: {
        // Identical except on failure: OP_TOUCH raises, OP_TOUCH_VALUE
        // hands the error object back as an ordinary value. Sharing the
        // path matters — the suspension logic below is the delicate part
        // and must not be duplicated.
        const bool as_value = (op == OP_TOUCH_VALUE);
        Value fut_val = f.pop();
        if (!Heap::is_future(fut_val)) {
          f.state = Fiber::State::Error;
          f.error_message = as_value
              ? "[VM Error] touch/or-error: expected a future"
              : "[VM Error] touch: expected a future";
          return StepResult::Error;
        }
        ObjFuture *fut = fut_val.as_ptr<ObjFuture>();
        if (fut->is_completed) {
          f.awaited = Value::nil();   // no longer waiting on anything
          if (fut->is_error) {
            if (!as_value) raise_failed_future(fut);
            // As a value: hand back a real error object, so the caller can
            // ask error-object? / error-object-message exactly as a guard
            // clause would have.
            Value msg = Heap::is_string(fut->result)
                ? fut->result
                : heap.make_string("awaited computation failed");
            push_temp_root(&msg);
            Value err = heap.make_error_object(msg, {});
            pop_temp_root();
            f.push(err);
            break;
          }
          f.push(fut->result);
          break;
        }

        // Nested inside a native call (load/map/apply/for-each/force):
        // this fiber's continuation includes C++ frames from
        // call_closure, so it physically cannot suspend — the same reason
        // (yield) is rejected there. Drive the computing fiber directly
        // instead. That is the old behaviour, kept ONLY where suspending
        // is impossible, and now null-safe and deadline-bounded instead
        // of an unbounded loop over a possibly-freed pointer.
        if (stop_at_depth > 0) {
          if (!fut->fiber) {
            // An EXTERNAL future can only be settled by the host event
            // loop, and we cannot return to it from here: this fiber's
            // continuation includes C++ frames (guard, map, apply,
            // for-each, load, force all call back in through
            // call_closure), so it physically cannot suspend.
            //
            // `guard` is the one that bites in practice — wrapping a GPU
            // call in an error handler is the obvious thing to write, and
            // it is exactly what cannot work. Say so, name the fix, and
            // do not pretend this is about map.
            f.state = Fiber::State::Error;
            f.error_message =
                "[VM Error] touch: cannot await a host-settled future here. "
                "This touch is inside guard/map/apply/for-each/load, whose "
                "continuation includes native frames, so the fiber cannot "
                "suspend and the event loop can never run to settle it. "
                "Await it in the fiber body directly, outside that form.";
            return StepResult::Error;
          }
          // Pump the whole scheduler, not just this future's fiber: what
          // we await may itself be waiting on a third fiber, and driving
          // one in isolation livelocks the chain. Everyone but us runs.
          while (!fut->is_completed) {
            if (deadline != NO_DEADLINE && std::chrono::steady_clock::now() >= deadline) {
              frame->ip = ip - 1;   // resumable: re-touch on the next slice
              f.push(fut_val);
              return StepResult::Preempted;
            }
            step_all_active_fibers(max_instructions, std::chrono::milliseconds::max(), &f);
            if (fut->is_completed) break;
            // Can anything still make progress? Every other fiber being
            // blocked on an unsettled future means no, and spinning would
            // just hang. Report it instead.
            bool progress_possible = false;
            for (Fiber *cand : active_fibers) {
              if (cand == &f) continue;
              if (!Heap::is_future(cand->awaited)) { progress_possible = true; break; }
              if (cand->awaited.as_ptr<ObjFuture>()->is_completed) { progress_possible = true; break; }
            }
            if (!progress_possible) {
              f.state = Fiber::State::Error;
              f.error_message = "[VM Error] touch: awaiting a future that nothing "
                                "can complete (deadlock)";
              return StepResult::Error;
            }
          }
          if (fut->is_error) {
            if (!as_value) raise_failed_future(fut);
            Value msg = Heap::is_string(fut->result)
                ? fut->result
                : heap.make_string("awaited computation failed");
            push_temp_root(&msg);
            Value err = heap.make_error_object(msg, {});
            pop_temp_root();
            f.push(err);
            break;
          }
          f.push(fut->result);
          break;
        }

        // Not ready. Previously this drove the child fiber inline —
        //   while (fut->fiber && ...) step_fiber(*fut->fiber, 1000);
        // which had three defects: it read fut->fiber without knowing the
        // scheduler may already have deleted it (use-after-free), it ran
        // unbounded with no deadline (a hole straight through the frame
        // budget), and it dereferenced a null fiber for any future the VM
        // is not itself computing.
        //
        // Instead: suspend, and let the scheduler make progress. Rewind to
        // re-execute this very OP_TOUCH on resume, with its operand pushed
        // back — so waking up re-tests the condition rather than trusting
        // anything cached. The fiber stays in active_fibers while blocked
        // (skipped, not removed) so it needs no second root set and the
        // collector keeps tracing its stack.
        f.push(fut_val);
        frame->ip = ip - 1;   // back to the OP_TOUCH byte itself
        f.awaited = fut_val;
        f.state = Fiber::State::Suspended;
        return StepResult::Yielded;
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
                                  std::chrono::milliseconds wall_clock_budget,
                                  Fiber *exclude) {
  auto deadline = (wall_clock_budget == std::chrono::milliseconds::max())
                      ? NO_DEADLINE
                      : std::chrono::steady_clock::now() + wall_clock_budget;
  size_t preempted_count = 0;

  // A fiber the deadline cut off mid-flight last call is owed an
  // exclusive resume: nothing else may step until it reaches its own
  // yield, so no sibling ever observes its half-finished work.
  if (preempted_fiber && preempted_fiber != exclude) {
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
        if (res == StepResult::Error && !f->error_message.empty()) {
          fiber_errors.push_back(f->error_message);
        }
        size_t pos = static_cast<size_t>(it - active_fibers.begin());
        active_fibers.erase(it);
        settle_backing_future(f);
        delete f;
        // Everything after pos shifted down one; keep the cursor on the
        // same fiber it was pointing at.
        if (round_cursor > pos) --round_cursor;
      }
      // it yielded (or finished) — the round may proceed below
    }
  }

  // Round-robin from where the last round left off. Visiting at most one
  // full lap keeps a single call bounded even with no deadline set, which
  // is what (run-fibers) relies on to make exactly one pass per round.
  const size_t lap = active_fibers.size();
  size_t visited = 0;
  while (visited < lap && !active_fibers.empty()) {
    // Deadline already spent: don't start another fiber against it
    // (its first check is 1024 instructions in — it would overshoot).
    // The cursor stays put, so the next call picks up exactly here — that
    // is what turns "too much work" into every fiber running slower
    // rather than a lucky prefix running at full rate and the rest never.
    if (deadline != NO_DEADLINE && std::chrono::steady_clock::now() >= deadline) {
      break;
    }
    if (round_cursor >= active_fibers.size()) round_cursor = 0;
    Fiber *f = active_fibers[round_cursor];
    if (f == exclude) {   // mid-dispatch above us; stepping it would recurse
      ++visited;
      ++round_cursor;
      if (round_cursor >= active_fibers.size()) round_cursor = 0;
      continue;
    }
    StepResult res = step_fiber(*f, instructions_per_fiber, deadline);
    ++visited;
    if (res == StepResult::Completed || res == StepResult::Error) {
      if (res == StepResult::Error && !f->error_message.empty()) {
        fiber_errors.push_back(f->error_message);
      }
      active_fibers.erase(active_fibers.begin() + round_cursor);
      settle_backing_future(f);
      delete f;
      // Successor shifted into this slot: leave the cursor where it is.
    } else if (res == StepResult::Preempted) {
      // First preemption ends the round: this fiber takes the exclusive-
      // resume slot, and no sibling may step past it — under an
      // instruction cap this serializes progress into atomic inter-yield
      // sections rather than interleaving mid-flight fibers (the old,
      // unsound behavior), and under a deadline the budget is spent
      // anyway. Advance past it so that once it finally yields, the round
      // resumes with its siblings rather than re-serving it immediately.
      ++preempted_count;
      preempted_fiber = f;
      ++round_cursor;
      if (round_cursor >= active_fibers.size()) round_cursor = 0;
      break;
    } else {
      ++round_cursor;
      if (round_cursor >= active_fibers.size()) round_cursor = 0;
    }
  }
  return preempted_count;
}

// --- unsigned 32-bit helpers ---------------------------------------
// Coerce any number to a u32 bit pattern: a negative fixnum is read as
// two's complement, a flonum is wrapped into [0, 2^32).
static inline uint32_t to_u32(Value v) {
  if (v.is_int()) return static_cast<uint32_t>(v.as_int());
  double d = v.as_real();
  double m = std::fmod(d, 4294967296.0);
  if (m < 0.0) m += 4294967296.0;
  return static_cast<uint32_t>(m);
}

// Return a u32 EXACTLY: fixnum where it fits, flonum above 2^31. Every
// u32 is exactly representable in a double, so nothing is lost and
// composing these operations round-trips.
static inline Value from_u32(uint32_t x) {
  if (x <= static_cast<uint32_t>(INT32_MAX)) {
    return Value::from_int(static_cast<int32_t>(x));
  }
  return Value::from_double(static_cast<double>(x));
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
// lib/*.scm compiled into the binary. Looked up by BASENAME so that
// (load "lib/wgsl.scm"), (load "wgsl.scm") and a bare name all resolve to
// the same entry — the browser has no directories to be relative to.
static const char *embedded_lib_source(const std::string &path) {
  size_t slash = path.find_last_of('/');
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  for (int i = 0; i < VX_EMBEDDED_LIB_COUNT; ++i) {
    if (base == VX_EMBEDDED_LIBS[i].name) return VX_EMBEDDED_LIBS[i].source;
  }
  return nullptr;
}

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
      // `res` STAYS ROOTED across the reversal. Dropping it here and
      // rooting only `forward` was a use-after-free: the reversal itself
      // allocates, that cons can collect, and the reversed-so-far chain in
      // `res` is reachable from nothing else — so the walk read freed
      // memory. Found by ASan on vx-test.scm, which allocates hard enough
      // to land a collection inside this loop.
      Value forward = Value::nil();
      vm.push_temp_root(&forward);
      while (Heap::is_cons(res)) {
        forward = vm.heap.cons(Heap::car(res), forward);
        res = Heap::cdr(res);
      }
      vm.pop_temp_root();   // forward
      vm.pop_temp_root();   // res
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
    // Same rooting fix as the 2-argument path above: `res` must survive
    // the reversal, which allocates.
    Value forward = Value::nil();
    vm.push_temp_root(&forward);
    while (Heap::is_cons(res)) {
      forward = vm.heap.cons(Heap::car(res), forward);
      res = Heap::cdr(res);
    }
    vm.pop_temp_root();   // forward
    vm.pop_temp_root();   // res
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

  // format — four directives and no more:
  //   ~a  display (human-readable, strings unquoted)
  //   ~s  write   (machine-readable, strings quoted)
  //   ~%  newline
  //   ~~  a literal tilde
  //
  // Deliberately NOT Common Lisp's FORMAT. That is a genuinely powerful
  // mini-language — ~{~a~^, ~} to emit a comma-separated argument list is
  // exactly what code generation wants — and also a famously write-only
  // one, with iteration, conditionals and argument-jumping. The iteration
  // directive is the thin end of that wedge; a `join` procedure written in
  // Scheme reads better than a directive nobody can remember.
  //
  // Destination follows the usual convention: a leading port writes there
  // and returns unspecified, otherwise the result is returned as a string.
  def_global("format", heap.make_subr("format", [](VM &vm, uint32_t argc, Value *args) -> Value {
    uint32_t i = 0;
    ObjPort *dest = nullptr;
    if (argc > 0 && Heap::is_port(args[0])) {
      dest = args[0].as_ptr<ObjPort>();
      i = 1;
    }
    if (i >= argc || !Heap::is_string(args[i])) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message =
            "[VM Error] format: expected a format string";
      }
      return Value::unspecified();
    }
    std::string_view fmt = args[i].as_ptr<ObjString>()->view();
    uint32_t next_arg = i + 1;
    std::ostringstream buf;
    for (size_t k = 0; k < fmt.size(); ++k) {
      if (fmt[k] != '~' || k + 1 == fmt.size()) { buf << fmt[k]; continue; }
      char d = fmt[++k];
      switch (d) {
        case 'a': case 'A':
          if (next_arg < argc) vm.display_value(args[next_arg++], buf);
          break;
        case 's': case 'S':
          if (next_arg < argc) buf << vm.format_value(args[next_arg++]);
          break;
        case '%': case 'n': buf << '\n'; break;
        case '~': buf << '~'; break;
        default:
          // Unknown directive: emit it verbatim rather than guessing.
          buf << '~' << d;
          break;
      }
    }
    if (dest) {
      if (dest->out) *dest->out << buf.str();
      return Value::unspecified();
    }
    return vm.heap.make_string(buf.str());
  }, 1, UINT32_MAX));

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

  // --- Bytes and views -----------------------------------------------
  // Untyped storage plus typed views. See the ObjBytes/ObjView comments for
  // why the element type lives on the view rather than the buffer.
  def_global("make-bytes", heap.make_subr("make-bytes", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!args[0].is_int() || args[0].as_int() < 0) {
      vm.raise_contract("make-bytes: expected a non-negative length, got " +
                        vm.format_value(args[0]));
    }
    return vm.heap.make_bytes(static_cast<size_t>(args[0].as_int()));
  }, 1, 1));

  def_global("open-byte-sink", heap.make_subr("open-byte-sink", [](VM &vm, uint32_t, Value *) -> Value {
    return vm.heap.make_byte_sink();
  }, 0, 0));

  def_global("bytes?", heap.make_subr("bytes?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_bytes(args[0]));
  }, 1, 1));

  def_global("bytes-length", heap.make_subr("bytes-length", [](VM &vm, uint32_t, Value *args) -> Value {
    ObjBytes *b = vm.require_bytes(args[0], "bytes-length");
    if (!b) return Value::unspecified();
    return Value::from_int(static_cast<int64_t>(b->data.size()));
  }, 1, 1));

  def_global("bytes-residency", heap.make_subr("bytes-residency", [](VM &vm, uint32_t, Value *args) -> Value {
    ObjBytes *b = vm.require_bytes(args[0], "bytes-residency");
    if (!b) return Value::unspecified();
    return Value::from_keyword_id(vm.intern(
        b->residency == ObjBytes::Residency::Building ? "building" : "sealed"));
  }, 1, 1));

  // Appending is legal ONLY while building. Once sealed, a buffer's address
  // is something a view — and eventually a GPU bind group — depends on, and
  // growing would reallocate underneath them. Making that an error turns a
  // silent-corruption bug into a message.
  def_global("bytes-append!", heap.make_subr("bytes-append!", [](VM &vm, uint32_t argc, Value *args) -> Value {
    ObjBytes *b = vm.require_bytes(args[0], "bytes-append!");
    if (!b) return Value::unspecified();
    if (b->residency != ObjBytes::Residency::Building) {
      vm.raise_contract("bytes-append!: buffer is sealed — appending would "
                        "reallocate it out from under every view bound to it");
    }
    for (uint32_t i = 1; i < argc; ++i) {
      if (Heap::is_string(args[i])) {
        std::string_view sv = args[i].as_ptr<ObjString>()->view();
        b->data.insert(b->data.end(), sv.begin(), sv.end());
      } else if (args[i].is_int()) {
        b->data.push_back(static_cast<uint8_t>(args[i].as_int() & 0xFF));
      } else if (Heap::is_bytes(args[i])) {
        const auto &src = args[i].as_ptr<ObjBytes>()->data;
        b->data.insert(b->data.end(), src.begin(), src.end());
      }
    }
    return Value::from_int(static_cast<int64_t>(b->data.size()));
  }, 1, UINT32_MAX));

  def_global("bytes-seal!", heap.make_subr("bytes-seal!", [](VM &vm, uint32_t, Value *args) -> Value {
    ObjBytes *b = vm.require_bytes(args[0], "bytes-seal!");
    if (!b) return Value::unspecified();
    b->residency = ObjBytes::Residency::Sealed;
    return args[0];
  }, 1, 1));

  def_global("bytes->string", heap.make_subr("bytes->string", [](VM &vm, uint32_t, Value *args) -> Value {
    ObjBytes *b = vm.require_bytes(args[0], "bytes->string");
    if (!b) return Value::unspecified();
    return vm.heap.make_string(
        std::string(reinterpret_cast<const char *>(b->data.data()), b->data.size()));
  }, 1, 1));

  // (bytes-view buffer type [offset stride]) — how to read these bytes.
  // Two views of different types may overlay the same buffer, which is what
  // makes an array-of-structs layout expressible without copying:
  //   (bytes-view b 'f32 0  32)   ; @P.x, every 32 bytes
  //   (bytes-view b 'f32 12 32)   ; @w,   same buffer, different offset
  def_global("bytes-view", heap.make_subr("bytes-view", [](VM &vm, uint32_t argc, Value *args) -> Value {
    ObjBytes *b = vm.require_bytes(args[0], "bytes-view");
    if (!b) return Value::unspecified();
    // Element types are KEYWORDS (:f32, :u32, ...) rather than quoted
    // symbols: they are tags from a closed set, never names you would
    // bind, so the quote in 'f32 would exist only to defend against a
    // variable capture that cannot happen. Self-evaluating reads better
    // in a DSL you type constantly, and matches how keywords already work
    // here as map keys and accessors.
    if (!args[1].is_keyword()) {
      vm.raise_contract("bytes-view: expected an element type keyword "
                        "(:u8, :i32, :u32, :f32, :f64), got " + vm.format_value(args[1]));
    }
    std::string t = vm.get_symbol_name(args[1].as_keyword_id());
    ElemType elem;
    if (t == "u8") elem = ElemType::U8;
    else if (t == "i32") elem = ElemType::I32;
    else if (t == "u32") elem = ElemType::U32;
    else if (t == "f32") elem = ElemType::F32;
    else if (t == "f64") elem = ElemType::F64;
    else {
      vm.raise_contract("bytes-view: unknown element type :" + t +
                        " (expected :u8, :i32, :u32, :f32 or :f64)");
    }
    uint32_t esz = elem_size(elem);
    uint32_t offset = (argc > 2 && args[2].is_int()) ? static_cast<uint32_t>(args[2].as_int()) : 0;
    uint32_t stride = (argc > 3 && args[3].is_int()) ? static_cast<uint32_t>(args[3].as_int()) : esz;
    if (stride < esz) stride = esz;
    size_t total = b->data.size();
    // How many whole elements fit, given where we start and how far apart
    // they are. Computed once here so ref/set! need only compare an index.
    uint32_t count = 0;
    if (total > offset) {
      size_t span = total - offset;
      if (span >= esz) count = static_cast<uint32_t>((span - esz) / stride + 1);
    }
    return vm.heap.make_view(args[0], offset, stride, count, elem);
  }, 2, 4));

  def_global("view?", heap.make_subr("view?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_view(args[0]));
  }, 1, 1));

  def_global("view-length", heap.make_subr("view-length", [](VM &vm, uint32_t, Value *args) -> Value {
    ObjView *v = vm.require_view(args[0], "view-length");
    if (!v) return Value::unspecified();
    return Value::from_int(static_cast<int64_t>(v->count));
  }, 1, 1));

  def_global("view-type", heap.make_subr("view-type", [](VM &vm, uint32_t, Value *args) -> Value {
    ObjView *v = vm.require_view(args[0], "view-type");
    if (!v) return Value::unspecified();
    const char *n = "u8";
    switch (v->elem) {
      case ElemType::U8:  n = "u8";  break;
      case ElemType::I32: n = "i32"; break;
      case ElemType::U32: n = "u32"; break;
      case ElemType::F32: n = "f32"; break;
      case ElemType::F64: n = "f64"; break;
    }
    return Value::from_keyword_id(vm.intern(n));
  }, 1, 1));

  def_global("view-bytes", heap.make_subr("view-bytes", [](VM &vm, uint32_t, Value *args) -> Value {
    ObjView *v = vm.require_view(args[0], "view-bytes");
    if (!v) return Value::unspecified();
    return v->bytes;
  }, 1, 1));

  def_global("view-ref", heap.make_subr("view-ref", [](VM &vm, uint32_t, Value *args) -> Value {
    ObjView *v = vm.require_view(args[0], "view-ref");
    if (!v) return Value::unspecified();
    if (!args[1].is_int() || args[1].as_int() < 0 ||
        static_cast<uint32_t>(args[1].as_int()) >= v->count) {
      vm.raise_contract(std::string("view-ref: index out of range (view holds ") +
                        std::to_string(v->count) + " elements), got " +
                        vm.format_value(args[1]));
    }
    const uint8_t *p = v->bytes.as_ptr<ObjBytes>()->data.data() +
                       v->offset + static_cast<size_t>(args[1].as_int()) * v->stride;
    switch (v->elem) {
      case ElemType::U8:  return Value::from_int(*p);
      case ElemType::I32: { int32_t x;  std::memcpy(&x, p, 4); return Value::from_int(x); }
      // A u32 above INT32_MAX has no fixnum to land in (fixnums are 32-bit
      // and signed), so it promotes to a flonum — the same rule the reader
      // and arithmetic use, rather than wrapping negative.
      case ElemType::U32: {
        uint32_t x; std::memcpy(&x, p, 4);
        if (x > static_cast<uint32_t>(INT32_MAX)) return Value::from_double(x);
        return Value::from_int(static_cast<int32_t>(x));
      }
      case ElemType::F32: { float x;    std::memcpy(&x, p, 4); return Value::from_double(x); }
      case ElemType::F64: { double x;   std::memcpy(&x, p, 8); return Value::from_double(x); }
    }
    return Value::unspecified();
  }, 2, 2));

  def_global("view-set!", heap.make_subr("view-set!", [](VM &vm, uint32_t, Value *args) -> Value {
    ObjView *v = vm.require_view(args[0], "view-set!");
    if (!v) return Value::unspecified();
    if (!args[1].is_int() || args[1].as_int() < 0 ||
        static_cast<uint32_t>(args[1].as_int()) >= v->count) {
      vm.raise_contract(std::string("view-set!: index out of range (view holds ") +
                        std::to_string(v->count) + " elements), got " +
                        vm.format_value(args[1]));
    }
    double d = args[2].is_int() ? static_cast<double>(args[2].as_int()) : args[2].as_real();
    uint8_t *p = v->bytes.as_ptr<ObjBytes>()->data.data() +
                 v->offset + static_cast<size_t>(args[1].as_int()) * v->stride;
    switch (v->elem) {
      case ElemType::U8:  *p = static_cast<uint8_t>(static_cast<int64_t>(d) & 0xFF); break;
      case ElemType::I32: { int32_t x  = static_cast<int32_t>(d);  std::memcpy(p, &x, 4); break; }
      case ElemType::U32: { uint32_t x = static_cast<uint32_t>(d); std::memcpy(p, &x, 4); break; }
      case ElemType::F32: { float x    = static_cast<float>(d);    std::memcpy(p, &x, 4); break; }
      case ElemType::F64: { double x   = d;                        std::memcpy(p, &x, 8); break; }
    }
    return Value::unspecified();
  }, 3, 3));

  // --- Handles to host-side objects ---------------------------------
  // A handle names something the VM cannot hold: a GPUDevice, a buffer, a
  // pipeline. Ownership is explicit because nothing else can be — the
  // collector will never call destroy(), and a finalizer would run at an
  // unpredictable time, which for a GPU resource is the same as not
  // running. See handle-release! for what that costs and buys.
  def_global("handle?", heap.make_subr("handle?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_handle(args[0]));
  }, 1, 1));

  def_global("handle-kind", heap.make_subr("handle-kind", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_handle(args[0])) return Value::boolean_false();
    return Value::from_keyword_id(args[0].as_ptr<ObjHandle>()->kind);
  }, 1, 1));

  def_global("handle-released?", heap.make_subr("handle-released?", [](VM &, uint32_t, Value *args) -> Value {
    if (!Heap::is_handle(args[0])) return Value::boolean_false();
    return Value::from_bool(args[0].as_ptr<ObjHandle>()->released);
  }, 1, 1));

  // Releasing marks the OBJECT, not the reference, which is exactly why a
  // handle is a heap object: every alias sees the release, so
  //   (let ((b buf)) (handle-release! buf) (use b))
  // fails loudly instead of using a destroyed GPU resource. Idempotent —
  // releasing twice is fine, since cleanup paths overlap in practice.
  def_global("handle-release!", heap.make_subr("handle-release!", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_handle(args[0])) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message =
            "[VM Error] handle-release!: contract violation, expected a handle, got " +
            vm.format_value(args[0]);
      }
      return Value::unspecified();
    }
    ObjHandle *h = args[0].as_ptr<ObjHandle>();
    if (h->released) return Value::boolean_false();   // already gone
    h->released = true;
    vm.release_host_handle(h->id);
    return Value::boolean_true();
  }, 1, 1));

  // --- String ports (R7RS / SRFI-6) ---------------------------------
  // The idiomatic Scheme answer to "build a string incrementally", and
  // the right backing for generating source text: one growable buffer,
  // linear appends, and every existing writer works on it because it is
  // just a port.
  def_global("open-output-string", heap.make_subr("open-output-string", [](VM &vm, uint32_t, Value *) -> Value {
    return vm.heap.make_output_string_port();
  }, 0, 0));

  def_global("get-output-string", heap.make_subr("get-output-string", [](VM &vm, uint32_t, Value *args) -> Value {
    ObjPort *p = Heap::is_port(args[0]) ? args[0].as_ptr<ObjPort>() : nullptr;
    if (!p || !p->oss) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message =
            "[VM Error] get-output-string: contract violation, expected a string "
            "output port, got " + vm.format_value(args[0]);
      }
      return Value::unspecified();
    }
    return vm.heap.make_string(p->oss->str());
  }, 1, 1));

  def_global("open-input-string", heap.make_subr("open-input-string", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!Heap::is_string(args[0])) {
      if (vm.current_fiber) {
        vm.current_fiber->state = Fiber::State::Error;
        vm.current_fiber->error_message =
            "[VM Error] open-input-string: contract violation, expected string, got " +
            vm.format_value(args[0]);
      }
      return Value::unspecified();
    }
    return vm.heap.make_input_string_port(std::string(args[0].as_ptr<ObjString>()->view()));
  }, 1, 1));

  // with-output-to-file, with-output-to-string, call-with-input-file,
  // call-with-output-file and call-with-output-string are defined in
  // lib/prelude.scm, which runs at the END of this function. They were
  // Scheme snippets embedded here; the prelude is the same idea with one
  // home, and running last means they can rely on every primitive rather
  // than only the ones defined above their old position.

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

  // R7RS: true for any port, in either direction. The two directional
  // predicates below existed without it, so asking "is this a port at all"
  // meant writing (or (input-port? x) (output-port? x)).
  def_global("port?", heap.make_subr("port?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_port(args[0]));
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

  // Push buffered output through without closing. Matters for ports whose
  // backing stream batches — the browser's sink ports buffer a line at a
  // time — where a `display` with no trailing newline would otherwise sit
  // unseen. A no-op on ports that don't buffer, so it is always safe.
  def_global("flush-output-port", heap.make_subr("flush-output-port", [](VM &vm, uint32_t argc, Value *args) -> Value {
    std::ostream *os = resolve_out(vm, argc, args, 0);
    if (os) os->flush();
    return Value::unspecified();
  }, 0, 1));

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
  // The family reads and/xor/not/ior; `or` is the name everyone reaches
  // for first. Alias, not a new operation.
  def_global("bitwise-or", heap.make_subr("bitwise-or", subr_logior, 0, UINT32_MAX));

  // --- unsigned 32-bit arithmetic ------------------------------------
  // The bitwise-* family above works on vxs fixnums, which are SIGNED
  // 32-bit — so (bitwise-xor 2147483648 1) comes back as -2147483647 and
  // (arithmetic-shift 1 31) wraps negative. That is defensible for
  // two's-complement integers and useless for anything that must match a
  // published bit-exact specification: a counter-based RNG, a hash, a
  // checksum.
  //
  // These take any number (negative fixnums are read as two's
  // complement, flonums are wrapped into range) and return an EXACT
  // unsigned result — a fixnum below 2^31, a flonum above it, since
  // every u32 is exactly representable in a double's 53-bit mantissa.
  // Composing them therefore round-trips, which the signed family does
  // not.
  def_global("u32", heap.make_subr("u32", [](VM &, uint32_t, Value *args) -> Value {
    return from_u32(to_u32(args[0]));
  }, 1, 1));

  def_global("u32+", heap.make_subr("u32+", [](VM &, uint32_t argc, Value *args) -> Value {
    uint32_t r = 0;
    for (uint32_t i = 0; i < argc; ++i) r += to_u32(args[i]);   // wraps, as intended
    return from_u32(r);
  }, 0, UINT32_MAX));

  def_global("u32*", heap.make_subr("u32*", [](VM &, uint32_t argc, Value *args) -> Value {
    uint32_t r = 1;
    for (uint32_t i = 0; i < argc; ++i) r *= to_u32(args[i]);
    return from_u32(r);
  }, 0, UINT32_MAX));

  def_global("u32-xor", heap.make_subr("u32-xor", [](VM &, uint32_t argc, Value *args) -> Value {
    uint32_t r = 0;
    for (uint32_t i = 0; i < argc; ++i) r ^= to_u32(args[i]);
    return from_u32(r);
  }, 0, UINT32_MAX));

  def_global("u32-and", heap.make_subr("u32-and", [](VM &, uint32_t argc, Value *args) -> Value {
    uint32_t r = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < argc; ++i) r &= to_u32(args[i]);
    return from_u32(r);
  }, 0, UINT32_MAX));

  def_global("u32-or", heap.make_subr("u32-or", [](VM &, uint32_t argc, Value *args) -> Value {
    uint32_t r = 0;
    for (uint32_t i = 0; i < argc; ++i) r |= to_u32(args[i]);
    return from_u32(r);
  }, 0, UINT32_MAX));

  def_global("u32-not", heap.make_subr("u32-not", [](VM &, uint32_t, Value *args) -> Value {
    return from_u32(~to_u32(args[0]));
  }, 1, 1));

  // Shifts are LOGICAL: no sign to extend, and a shift of 32 or more is
  // zero rather than undefined (C++ would leave that up to the hardware).
  def_global("u32-shl", heap.make_subr("u32-shl", [](VM &, uint32_t, Value *args) -> Value {
    uint32_t n = to_u32(args[1]);
    return from_u32(n >= 32 ? 0u : (to_u32(args[0]) << n));
  }, 2, 2));

  def_global("u32-shr", heap.make_subr("u32-shr", [](VM &, uint32_t, Value *args) -> Value {
    uint32_t n = to_u32(args[1]);
    return from_u32(n >= 32 ? 0u : (to_u32(args[0]) >> n));
  }, 2, 2));

  // Rotate left. The one operation Threefry is built out of, and the one
  // C++ makes easy to write wrongly: x << 32 is undefined behaviour, so
  // the n % 32 == 0 case must be handled rather than assumed.
  def_global("u32-rotl", heap.make_subr("u32-rotl", [](VM &, uint32_t, Value *args) -> Value {
    uint32_t x = to_u32(args[0]);
    uint32_t n = to_u32(args[1]) & 31u;
    return from_u32(n == 0 ? x : ((x << n) | (x >> (32 - n))));
  }, 2, 2));

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
    if (radix != 10 && radix >= 2 && radix <= 36) {
      // Both exact fixnums and INTEGRAL flonums format here. Every u32
      // value above 2^31 is a flonum in this system, and this path used
      // to accept only fixnums — so (number->string x 16) on one of them
      // fell through and silently returned DECIMAL text, ignoring the
      // radix it was handed. Formatting it is the useful answer; note
      // that R7RS leaves inexact + non-decimal radix undefined, and
      // exactness does not round-trip through it below 2^31.
      bool integral = false;
      int64_t iv = 0;
      if (args[0].is_int()) {
        iv = args[0].as_int();
        integral = true;
      } else if (args[0].is_double()) {
        double d = args[0].as_double();
        // 2^53 is where consecutive integers stop being representable.
        if (d == std::floor(d) && std::fabs(d) <= 9007199254740992.0) {
          iv = static_cast<int64_t>(d);
          integral = true;
        }
      }
      if (integral) {
        bool neg = iv < 0;
        uint64_t uv = neg ? static_cast<uint64_t>(-iv)
                          : static_cast<uint64_t>(iv);
        static const char DIGITS[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        std::string digits;
        if (uv == 0) {
          digits = "0";
        } else {
          while (uv > 0) {
            digits.push_back(DIGITS[uv % static_cast<uint64_t>(radix)]);
            uv /= static_cast<uint64_t>(radix);
          }
          std::reverse(digits.begin(), digits.end());
        }
        if (neg) digits.insert(digits.begin(), '-');
        return vm.heap.make_string(digits);
      }
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
      // Magnitudes past our 32-bit exact integers promote to flonum here,
      // exactly as the radix-10 path and the READER already do. This used
      // to return #f on the theory that R4RS only requires exact parsing
      // outside radix 10 — true, but it made (string->number "ffffffff" 16)
      // fail while the equivalent decimal succeeded, and every u32 value
      // above 2^31 is precisely such a magnitude. The u32 layer represents
      // those as flonums throughout; refusing to read them back was the
      // one place that disagreed.
      char *end = nullptr;
      errno = 0;
      long long iv = std::strtoll(sv.data(), &end, radix);
      if (end != sv.data() + sv.size() || errno == ERANGE) {
        return Value::boolean_false();
      }
      if (iv >= INT32_MIN && iv <= INT32_MAX) {
        return Value::from_int(static_cast<int32_t>(iv));
      }
      return Value::from_double(static_cast<double>(iv));
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
  // themselves (see yesterday's for-each fix — vxs follows
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

  // (vm-stats) -> association list of runtime counters. Same numbers the
  // wasm build serves over vxs_stats_json, available here so allocation
  // behaviour is measurable from a plain CLI benchmark instead of being
  // inferred from wall-clock deltas.
  def_global("vm-stats", heap.make_subr("vm-stats", [](VM &vm, uint32_t, Value *) -> Value {
    GCGuard guard(vm.heap);
    auto entry = [&vm](const char *name, size_t v) {
      return vm.heap.cons(vm.heap.cons(Value::from_symbol_id(vm.intern(name)),
                                       Value::from_double(static_cast<double>(v))),
                          Value::nil());
    };
    // Built back-to-front so the list reads in the order written here.
    Value list = Value::nil();
    auto push = [&](const char *name, size_t v) {
      Value cell = entry(name, v);
      Heap::set_cdr(cell, list);
      list = cell;
    };
    push("active-fibers", vm.active_fibers.size());
    push("gc-last-freed", vm.heap.get_last_gc_freed());
    push("gc-count", vm.heap.get_gc_count());
    push("gc-threshold", vm.heap.get_gc_threshold());
    push("total-objects-freed", vm.heap.get_total_objects_freed());
    push("total-objects-allocated", vm.heap.get_total_objects_allocated());
    push("total-bytes-allocated", vm.heap.get_total_bytes_allocated());
    push("live-objects", vm.heap.get_live_objects());
    push("live-bytes", vm.heap.get_bytes_allocated());
    return list;
  }, 0, 0));

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

    std::string code;
    if (file.is_open()) {
      std::stringstream buf;
      buf << file.rdbuf();
      code = buf.str();
    } else {
      // Fall back to the copy compiled into the binary. This is what makes
      // (load "lib/wgsl.scm") mean the same thing in the browser, which has
      // no filesystem at all, as it does natively. On-disk wins when it
      // exists, so editing a lib and re-running natively picks up the edit
      // without a rebuild.
      const char *embedded = embedded_lib_source(filename);
      if (!embedded) {
        if (vm.current_fiber) {
          vm.current_fiber->state = Fiber::State::Error;
          vm.current_fiber->error_message = "[VM Error] load: cannot open file " + filename;
        }
        return Value::unspecified();
      }
      code = embedded;
    }
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

  // The SYSTEM is vxs; `vx-scheme` is only the command-line binary's name.
  def_global("scheme-implementation-type", heap.make_subr("scheme-implementation-type", [](VM &vm, uint32_t, Value *) -> Value {
    return Value::from_symbol_id(vm.intern("vxs"));
  }, 0, 0));

  def_global("exit", heap.make_subr("exit", [](VM &, uint32_t argc, Value *args) -> Value {
    int code = (argc > 0 && args[0].is_int()) ? args[0].as_int() : 0;
    std::exit(code);
  }, 0, 1));

  def_global("scheme-implementation-platform", heap.make_subr("scheme-implementation-platform", [](VM &vm, uint32_t, Value *) -> Value {
    return Value::from_symbol_id(vm.intern("native"));
  }, 0, 0));

  def_global("vxs-implementation-type", heap.make_subr("vxs-implementation-type", [](VM &vm, uint32_t, Value *) -> Value {
    return Value::from_symbol_id(vm.intern("vm"));
  }, 0, 0));

  def_global("defined?", heap.make_subr("defined?", [](VM &vm, uint32_t, Value *args) -> Value {
    if (!args[0].is_symbol() && !Heap::is_string(args[0])) return Value::boolean_false();
    std::string sym = args[0].is_symbol() ? vm.get_symbol_name(args[0].as_symbol_id()) : std::string(args[0].as_ptr<ObjString>()->view());
    return Value::from_bool(vm.globals.find(sym) != vm.globals.end());
  }, 1, 1));

  // Deliberately BROAD: it answers "can this be applied?", and vectors,
  // maps and keywords are all applicable in vxs. A second, narrower
  // definition (closures and subrs only) used to sit ~130 lines above this
  // one, dead — this override always won. Removed rather than reconciled,
  // because breadth is the answer callers want.
  //
  // The one place that must NOT use it is `force`, which needs "is this a
  // delayed computation?" — a different question, and the reason force
  // tests is_closure directly. See the comment there.
  def_global("procedure?", heap.make_subr("procedure?", [](VM &, uint32_t, Value *args) -> Value {
    return Value::from_bool(Heap::is_closure(args[0]) || Heap::is_subr(args[0]) || args[0].is_keyword() || Heap::is_map(args[0]) || Heap::is_vector(args[0]));
  }, 1, 1));

  // THE definition of force — do not add a Scheme one to the prelude. One
  // used to exist as a bootstrap snippet earlier in this function, reading
  // (if (procedure? p) (p) p), and this def_global silently overrode it.
  // Keeping it that way is correct rather than merely conservative:
  // `procedure?` here is broad enough to include vectors, maps and
  // keywords, so the Scheme form calls anything callable instead of
  // returning it — (force (vector 1 2 3)) gives () rather than the vector.
  // is_closure is the right question. Locked down by layer 14.
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

  // The prelude runs LAST, so it sees every primitive defined above.
  // Skipped entirely under --no-prelude, which leaves the bare kernel:
  // compiler special forms and C++ primitives, nothing else.
  if (prelude_enabled) {
    Reader r(*this, embedded_lib_source("prelude.scm"));
    while (true) {
      Value form = r.read_form();
      if (form.is_eof()) break;
      Compiler comp(*this);
      ObjClosure *cl = comp.compile_top_level(form);
      call_closure(cl, {});
    }
  }

  // Sweep any temporary artifacts from bootstrap evaluation
  collect_garbage();
}

} // namespace vxs
