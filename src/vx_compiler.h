#pragma once

#include "vx_heap.h"
#include "vx_reader.h"
#include "vx_value.h"
#include "vx_vm.h"
#include <stdexcept>
#include <string>
#include <vector>

namespace vxs {

// Build-time escape hatch used only for A/B benchmarking the inline `let`
// path against the old closure-per-scope compilation. Defaults to enabled.
#ifndef VXS_INLINE_LET_DISABLED
#define VXS_INLINE_LET_DISABLED 0
#endif

struct Local {
  std::string name;
  int depth;
  bool is_captured;
};

struct UpvalueDesc {
  uint8_t index;
  bool is_local;
};

// (quote expr) materializes its operand rather than embedding it verbatim:
// [e1 e2 ...]/{k1 v1 ...} desugar to (vector ...)/(hash-map ...) call forms
// at read time (so they evaluate their elements when used as an
// expression — see the comment in vx_reader.h), which means a bare quote
// would otherwise just return that inert call-form list instead of a real
// vector/map. Recursively rebuilds any such sub-form (at any nesting
// depth, including inside an already-literal #(...) vector) into the real
// object it denotes, quoting each element in turn — mirrors the
// (pre-existing) convention expand_quasiquote uses for the same shape.
//
// A free function (not a Compiler method) so the `read` primitive can
// apply the same materialization to data parsed at runtime — read has no
// compilation pass of its own, so without this, reading back a [...] a
// program itself just wrote would silently hand back a (vector ...) list
// instead of a real vector.
inline Value quote_materialize(VM &vm, Value form) {
  if (Heap::is_vector(form)) {
    ObjVector *vec = form.as_ptr<ObjVector>();
    Value new_vec = vm.heap.make_vector(vec->size);
    ObjVector *nv = new_vec.as_ptr<ObjVector>();
    for (uint32_t i = 0; i < vec->size; ++i) {
      nv->set(i, quote_materialize(vm, vec->get(i)));
    }
    return new_vec;
  }
  if (Heap::is_map(form)) {
    ObjMap *m = form.as_ptr<ObjMap>();
    std::vector<std::pair<Value, Value>> entries;
    for (const auto &p : m->entries) {
      entries.push_back({quote_materialize(vm, p.first), quote_materialize(vm, p.second)});
    }
    return vm.heap.make_map(entries);
  }
  if (Heap::is_cons(form)) {
    Value head = Heap::car(form);
    if (head.is_symbol()) {
      std::string sym = vm.get_symbol_name(head.as_symbol_id());
      if (sym == "%bracket-vector") {
        std::vector<Value> elems;
        Value cur = Heap::cdr(form);
        while (Heap::is_cons(cur)) {
          elems.push_back(quote_materialize(vm, Heap::car(cur)));
          cur = Heap::cdr(cur);
        }
        Value vec = vm.heap.make_vector(static_cast<uint32_t>(elems.size()));
        ObjVector *ov = vec.as_ptr<ObjVector>();
        for (size_t i = 0; i < elems.size(); ++i) {
          ov->set(static_cast<uint32_t>(i), elems[i]);
        }
        return vec;
      }
      if (sym == "%brace-map") {
        std::vector<std::pair<Value, Value>> entries;
        Value cur = Heap::cdr(form);
        while (Heap::is_cons(cur) && Heap::is_cons(Heap::cdr(cur))) {
          entries.push_back({quote_materialize(vm, Heap::car(cur)),
                             quote_materialize(vm, Heap::car(Heap::cdr(cur)))});
          cur = Heap::cdr(Heap::cdr(cur));
        }
        return vm.heap.make_map(entries);
      }
    }
    return vm.heap.cons(quote_materialize(vm, head),
                        quote_materialize(vm, Heap::cdr(form)));
  }
  return form;
}

class Compiler {
public:
  Compiler(VM &vm, Compiler *parent = nullptr)
      : vm(vm), parent(parent), scope_depth(0), max_locals(1) {}

  // --- desugaring vocabulary -------------------------------------------
  // Building source fragments out of raw vm.heap.cons/Value::from_symbol_id
  // buries the shape of the code being generated under punctuation. These
  // exist so a desugar reads roughly like the Scheme it produces.
  //
  // No rooting needed: compile_top_level holds a GCGuard for the whole
  // compilation, so nothing built here can be collected mid-construction.
  // (Do not lift these into runtime code, where that is not true.)

  // A symbol Value from an id — pairs with VM::sym, e.g. sym(vm.sym.s_let).
  static Value sym(uint32_t id) { return Value::from_symbol_id(id); }

  Value cons(Value a, Value d) { return vm.heap.cons(a, d); }

  // list(a, b, c) => (a b c). Variadic rather than an initializer_list so
  // the elements keep their own types and nothing needs a cast at the call
  // site. Plain C++11 — no C++20 required, pleasingly.
  template <typename... Vs>
  Value list(Vs... vs) {
    Value items[] = {vs..., Value::nil()};   // trailing nil keeps [] non-empty
    Value r = Value::nil();
    for (size_t i = sizeof...(Vs); i-- > 0;) r = vm.heap.cons(items[i], r);
    return r;
  }

  Value list() { return Value::nil(); }

  // (f . rest) — a call form whose argument list is already built.
  Value call_form(Value head, Value rest) { return vm.heap.cons(head, rest); }

  // (a b c) + tail => (a b c tail). Rebuilding a list with one element
  // appended is otherwise five lines of vector-and-reverse, three times
  // over in this file.
  Value append_last(Value lst, Value tail) {
    std::vector<Value> items;
    for (Value c = lst; Heap::is_cons(c); c = Heap::cdr(c)) {
      items.push_back(Heap::car(c));
    }
    items.push_back(tail);
    Value r = Value::nil();
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
      r = vm.heap.cons(*it, r);
    }
    return r;
  }

  ObjClosure *compile_top_level(Value form) {
    GCGuard guard(vm.heap);
    BytecodeChunk *chunk = new BytecodeChunk();
    add_local("<top-level>");
    compile_expr(form, *chunk, false);
    chunk->code.push_back(OP_RETURN);
    return vm.heap.allocate<ObjClosure>(chunk, 0, false, 0,
                                        static_cast<uint32_t>(max_locals));
  }

  ObjClosure *compile_function(Value params, Value body, uint32_t &out_arity,
                               std::vector<UpvalueDesc> &out_upvals,
                               const std::string &self_name = "") {
    BytecodeChunk *chunk = new BytecodeChunk();
    Compiler fn_compiler(vm, this);
    fn_compiler.scope_depth = 1;

    // Slot 0 is the callee closure itself (for self-recursion like named let)
    fn_compiler.add_local(self_name.empty() ? "<callee>" : self_name);

    // Register parameter locals (slots 1..N)
    uint32_t arity = 0;
    bool is_variadic = false;
    Value p = params;
    if (p.is_symbol()) {
      // (lambda args body...)
      std::string name = vm.get_symbol_name(p.as_symbol_id());
      fn_compiler.add_local(name);
      is_variadic = true;
      arity = 0;
    } else {
      while (Heap::is_cons(p)) {
        Value sym_val = Heap::car(p);
        assert(sym_val.is_symbol());
        std::string name = vm.get_symbol_name(sym_val.as_symbol_id());
        fn_compiler.add_local(name);
        ++arity;
        p = Heap::cdr(p);
      }
      if (p.is_symbol()) {
        // Dotted variadic: (lambda (a b . rest) body...)
        std::string name = vm.get_symbol_name(p.as_symbol_id());
        fn_compiler.add_local(name);
        is_variadic = true;
      }
    }
    out_arity = arity;

    // Scan leading internal defines: (define x e) or (define (f ...) ...)
    Value b = body;
    std::vector<std::pair<Value, Value>> internal_defs;
    while (Heap::is_cons(b)) {
      Value form = Heap::car(b);
      if (Heap::is_cons(form) && Heap::car(form).is_symbol() &&
          vm.get_symbol_name(Heap::car(form).as_symbol_id()) == "define") {
        Value rest = Heap::cdr(form);
        Value target = Heap::car(rest);
        Value def_body = Heap::cdr(rest);
        if (target.is_symbol()) {
          internal_defs.push_back({target, Heap::car(def_body)});
        } else if (Heap::is_cons(target)) {
          Value fn_name = Heap::car(target);
          Value fn_args = Heap::cdr(target);
          Value lambda_form =
              vm.heap.cons(Value::from_symbol_id(vm.intern("lambda")),
                           vm.heap.cons(fn_args, def_body));
          internal_defs.push_back({fn_name, lambda_form});
        }
        b = Heap::cdr(b);
      } else {
        break;
      }
    }

    if (!internal_defs.empty()) {
      // Desugar internal defines to (letrec ((v1 e1)...) b...)
      Value bindings = Value::nil();
      for (auto it = internal_defs.rbegin(); it != internal_defs.rend(); ++it) {
        Value pair =
            vm.heap.cons(it->first, vm.heap.cons(it->second, Value::nil()));
        bindings = vm.heap.cons(pair, bindings);
      }
      Value letrec_form =
          vm.heap.cons(Value::from_symbol_id(vm.intern("letrec")),
                       vm.heap.cons(bindings, b));
      fn_compiler.compile_expr(letrec_form, *chunk, true);
    } else {
      // Compile body forms
      if (b.is_nil()) {
        chunk->code.push_back(OP_UNSPECIFIED);
      } else {
        while (Heap::is_cons(b)) {
          bool is_last = Heap::cdr(b).is_nil();
          fn_compiler.compile_expr(Heap::car(b), *chunk, is_last);
          if (!is_last) {
            chunk->code.push_back(OP_POP);
          }
          b = Heap::cdr(b);
        }
      }
    }
    chunk->code.push_back(OP_RETURN);
    out_upvals = fn_compiler.upvalues;
    return vm.heap.allocate<ObjClosure>(
        chunk, arity, is_variadic, static_cast<uint32_t>(out_upvals.size()),
        static_cast<uint32_t>(fn_compiler.max_locals));
  }

private:
  void compile_expr(Value form, BytecodeChunk &chunk, bool is_tail = false) {
    // 1. Self-evaluating literals
    if (form.is_int() || form.is_double() || form.is_bool() || form.is_nil() ||
        form.is_unspecified() || form.is_eof() || form.is_char() ||
        form.is_keyword() || Heap::is_string(form) || Heap::is_vector(form) ||
        Heap::is_map(form)) {
      emit_constant(form, chunk);
      return;
    }

    // 2. Variable reference
    if (form.is_symbol()) {
      std::string name = vm.get_symbol_name(form.as_symbol_id());
      int local = resolve_local(name);
      if (local != -1) {
        emit_op(OP_GET_LOCAL, static_cast<uint16_t>(local), chunk);
      } else {
        int upval = resolve_upvalue(name);
        if (upval != -1) {
          emit_op(OP_GET_UPVALUE, static_cast<uint16_t>(upval), chunk);
        } else {
          uint16_t const_ix = add_constant(form, chunk);
          emit_op(OP_GET_GLOBAL, const_ix, chunk);
        }
      }
      return;
    }

    // 3. Compound form (list)
    if (Heap::is_cons(form)) {
      Value head = Heap::car(form);
      Value rest = Heap::cdr(form);

      if (head.is_symbol()) {
        std::string op_name = vm.get_symbol_name(head.as_symbol_id());

        // (quote expr)
        if (op_name == "quote") {
          emit_constant(quote_materialize(vm, Heap::car(rest)), chunk);
          return;
        }

        // (define var val) or (define (f args...) body...)
        if (op_name == "define") {
          Value target = Heap::car(rest);
          Value def_body = Heap::cdr(rest);

          if (target.is_symbol()) {
            compile_expr(Heap::car(def_body), chunk, false);
            uint16_t name_ix = add_constant(target, chunk);
            emit_op(OP_DEF_GLOBAL, name_ix, chunk);
            chunk.code.push_back(OP_UNSPECIFIED);
            return;
          } else if (Heap::is_cons(target)) {
            // (define (fn args...) body...) => (define fn (lambda (args...)
            // body...))
            Value fn_name = Heap::car(target);
            Value fn_args = Heap::cdr(target);
            uint32_t arity = 0;
            std::vector<UpvalueDesc> upvals;
            ObjClosure *closure =
                compile_function(fn_args, def_body, arity, upvals);
            emit_closure(closure, upvals, chunk);
            uint16_t name_ix = add_constant(fn_name, chunk);
            emit_op(OP_DEF_GLOBAL, name_ix, chunk);
            chunk.code.push_back(OP_UNSPECIFIED);
            return;
          }
        }

        // (set! var val)
        if (op_name == "set!") {
          Value var_sym = Heap::car(rest);
          Value val_expr = Heap::car(Heap::cdr(rest));
          compile_expr(val_expr, chunk, false);
          std::string name = vm.get_symbol_name(var_sym.as_symbol_id());
          int local = resolve_local(name);
          if (local != -1) {
            emit_op(OP_SET_LOCAL, static_cast<uint16_t>(local), chunk);
          } else {
            int upval = resolve_upvalue(name);
            if (upval != -1) {
              emit_op(OP_SET_UPVALUE, static_cast<uint16_t>(upval), chunk);
            } else {
              uint16_t name_ix = add_constant(var_sym, chunk);
              emit_op(OP_SET_GLOBAL, name_ix, chunk);
            }
          }
          return;
        }

        // (if test then [else])
        if (op_name == "if") {
          Value test_expr = Heap::car(rest);
          Value then_expr = Heap::car(Heap::cdr(rest));
          Value else_expr = Heap::cdr(Heap::cdr(rest));

          compile_expr(test_expr, chunk, false);

          size_t jump_false_ix = chunk.code.size();
          chunk.code.push_back(OP_JUMP_IF_FALSE);
          chunk.code.push_back(0); // placeholder
          chunk.code.push_back(0);

          // Then branch
          compile_expr(then_expr, chunk, is_tail);

          size_t jump_exit_ix = chunk.code.size();
          chunk.code.push_back(OP_JUMP);
          chunk.code.push_back(0);
          chunk.code.push_back(0);

          // Patch jump_false
          size_t else_target = chunk.code.size();
          patch_jump(jump_false_ix + 1,
                     static_cast<uint16_t>(else_target - (jump_false_ix + 3)),
                     chunk);

          // Else branch
          if (Heap::is_cons(else_expr)) {
            compile_expr(Heap::car(else_expr), chunk, is_tail);
          } else {
            chunk.code.push_back(OP_UNSPECIFIED);
          }

          // Patch jump_exit
          size_t exit_target = chunk.code.size();
          patch_jump(jump_exit_ix + 1,
                     static_cast<uint16_t>(exit_target - (jump_exit_ix + 3)),
                     chunk);
          return;
        }

        // (begin e1 e2 ...)
        if (op_name == "begin") {
          Value cur = rest;
          if (cur.is_nil()) {
            chunk.code.push_back(OP_UNSPECIFIED);
            return;
          }
          while (Heap::is_cons(cur)) {
            bool is_last = Heap::cdr(cur).is_nil();
            compile_expr(Heap::car(cur), chunk, is_last && is_tail);
            if (!is_last) {
              chunk.code.push_back(OP_POP);
            }
            cur = Heap::cdr(cur);
          }
          return;
        }

        // (delay expr) - Standard R4RS/R5RS Memoized Promise
        if (op_name == "delay") {
          // Reentrant-safe memoization — R4RS's own reference algorithm,
          // not the naive "evaluate straight into res" version this used
          // to desugar to. The difference only shows up when forcing a
          // promise's own expression forces that SAME promise again
          // before the outer call finishes (r4rstest.scm section 6 9
          // exercises exactly this: a promise whose body forces itself
          // once, using a flag to avoid infinite recursion). Naively,
          // the outer call's own `(set! res expr)` — evaluated only
          // AFTER the inner force call already ran and memoized its own
          // (different, correct) answer — unconditionally overwrites
          // that answer with whatever the outer call's copy of the same
          // expression happened to compute, one level further recursed.
          // The fix: evaluate into a temp, then re-check `done` before
          // committing — if a reentrant force already completed while
          // this call was busy evaluating, that memoized value wins and
          // this call's own (now-redundant) result is discarded.
          Value done_sym = Value::from_symbol_id(
              vm.intern("$done__" + std::to_string(vm.next_gensym_id++)));
          Value res_sym = Value::from_symbol_id(
              vm.intern("$res__" + std::to_string(vm.next_gensym_id++)));
          Value tmp_sym = Value::from_symbol_id(
              vm.intern("$tmp__" + std::to_string(vm.next_gensym_id++)));
          Value expr = Heap::car(rest);

          // (let ((done #f) (res #f) (tmp #f))
          //   (lambda ()
          //     (if (not done)
          //         (begin (set! tmp expr)
          //                (if (not done)                 ; re-check
          //                    (begin (set! res tmp) (set! done #t)))))
          //     res))
          Value F = Value::boolean_false();
          Value bindings = list(list(done_sym, F), list(res_sym, F),
                                list(tmp_sym, F));

          Value not_done = list(sym(vm.sym.s_not), done_sym);
          Value set_tmp  = list(sym(vm.sym.s_set), tmp_sym, expr);
          Value set_res  = list(sym(vm.sym.s_set), res_sym, tmp_sym);
          Value set_done = list(sym(vm.sym.s_set), done_sym, Value::boolean_true());

          Value commit     = list(sym(vm.sym.s_begin), set_res, set_done);
          Value recheck_if = list(sym(vm.sym.s_if), not_done, commit);
          Value body       = list(sym(vm.sym.s_begin), set_tmp, recheck_if);
          Value if_form    = list(sym(vm.sym.s_if), not_done, body);

          Value lambda_form = list(sym(vm.sym.s_lambda), Value::nil(),
                                   if_form, res_sym);
          Value let_form    = list(sym(vm.sym.s_let), bindings, lambda_form);

          compile_expr(let_form, chunk, is_tail);
          return;
        }

        // (lambda (params...) body...)
        if (op_name == "lambda") {
          Value params = Heap::car(rest);
          Value body = Heap::cdr(rest);
          uint32_t arity = 0;
          std::vector<UpvalueDesc> upvals;
          ObjClosure *closure = compile_function(params, body, arity, upvals);
          emit_closure(closure, upvals, chunk);
          return;
        }

        // (future expr)
        if (op_name == "future") {
          uint32_t arity = 0;
          std::vector<UpvalueDesc> upvals;
          ObjClosure *closure =
              compile_function(Value::nil(), rest, arity, upvals);
          emit_closure(closure, upvals, chunk);
          chunk.code.push_back(OP_FUTURE);
          return;
        }

        // (touch fut)
        if (op_name == "touch") {
          compile_expr(Heap::car(rest), chunk, false);
          chunk.code.push_back(OP_TOUCH);
          return;
        }

        // (touch/or-error future) — the value, or the error object if the
        // future failed. Never raises, so it needs no handler, so it works
        // where `guard` cannot: awaiting anything host-settled. Compiled
        // inline like touch (it must be able to suspend), never a subr.
        if (op_name == "touch/or-error") {
          compile_expr(Heap::car(rest), chunk, false);
          chunk.code.push_back(OP_TOUCH_VALUE);
          return;
        }

        // (yield)
        if (op_name == "yield") {
          chunk.code.push_back(OP_YIELD);
          chunk.code.push_back(OP_UNSPECIFIED);
          return;
        }

        // (unwind-protect body cleanup...) — CL semantics, honestly
        // named: body's value is the result; the cleanups run on EVERY
        // exit — normal return, escape continuation, or fiber error.
        // (Deliberately NOT dynamic-wind: that name promises symmetric
        // re-entry semantics for a control regime — re-entrant call/cc —
        // this VM deliberately doesn't have. dynamic-wind isn't even
        // R4RS; it arrived in R5RS. We decline the transplant.)
        //
        // Compiled inline, not as a subr: the body runs in the fiber's
        // own dispatch loop, so (yield) inside it is legal — the pending
        // cleanup lives on the fiber's winder list (Fiber::winders) and
        // survives suspension. The cleanups compile as a zero-arg
        // closure (full upvalue capture, via the ordinary lambda path);
        // OP_PUSH_WINDER parks it, and on normal exit OP_POP_WINDER /
        // OP_CALL 0 / OP_POP run it inline and discard its value. The
        // involuntary exits (escape, error) run parked winders via
        // VM::run_pending_winders. Note the body is never in tail
        // position — true of finally-like forms in every language.
        if (op_name == "unwind-protect") {
          Value body = Heap::car(rest);
          Value cleanups = Heap::cdr(rest);
          Value cleanup_lambda = vm.heap.cons(
              Value::from_symbol_id(vm.intern("lambda")),
              vm.heap.cons(Value::nil(), cleanups));
          compile_expr(cleanup_lambda, chunk, false); // closure on stack
          chunk.code.push_back(OP_PUSH_WINDER);
          compile_expr(body, chunk, false);           // body value on stack
          chunk.code.push_back(OP_POP_WINDER);        // cleanup closure back
          chunk.code.push_back(OP_CALL);
          chunk.code.push_back(0);
          chunk.code.push_back(OP_POP);               // drop cleanup's value
          return;
        }

        // (guard (var clause...) body...) — R7RS structured exception
        // handling. Desugars to (%guard (lambda (var) (cond clause...))
        // (lambda () body...)) — inherits cond's own `=>` support for
        // free, since the handler body genuinely IS a cond. If none of
        // the clauses match and the user didn't supply their own
        // (else ...), a trailing (else (raise var)) is appended: an
        // unhandled guard re-raises (per R7RS) into the guard's own
        // dynamic context, which for us just falls out of raising again
        // from inside %guard's handler call — by then an ordinary,
        // non-try-scoped C++ call, so it propagates outward exactly like
        // any other raise. See %guard's own comment (vx_vm.cpp) for why
        // this is a subr (yield-illegal inside, like call/cc) rather
        // than inline like unwind-protect: it needs an actual C++ catch
        // boundary, which only a nested call can provide.
        if (op_name == "guard") {
          Value spec = Heap::car(rest); // (var clause...)
          Value var = Heap::car(spec);
          Value clauses = Heap::cdr(spec);
          Value body = Heap::cdr(rest);

          bool has_else = false;
          for (Value c = clauses; Heap::is_cons(c); c = Heap::cdr(c)) {
            Value clause = Heap::car(c);
            if (Heap::is_cons(clause) && Heap::car(clause).is_symbol() &&
                vm.get_symbol_name(Heap::car(clause).as_symbol_id()) == "else") {
              has_else = true;
            }
          }

          Value cond_clauses = clauses;
          if (!has_else) {
            Value raise_call  = list(sym(vm.sym.s_raise), var);   // (raise var)
            Value else_clause = list(sym(vm.sym.s_else), raise_call);
            std::vector<Value> cs;
            for (Value c = clauses; Heap::is_cons(c); c = Heap::cdr(c)) {
              cs.push_back(Heap::car(c));
            }
            cs.push_back(else_clause);
            Value built = Value::nil();
            for (auto it = cs.rbegin(); it != cs.rend(); ++it) {
              built = vm.heap.cons(*it, built);
            }
            cond_clauses = built;
          }

          // (%guard (lambda (var) (cond clause...))
          //         (lambda ()    body...))
          Value cond_form      = call_form(sym(vm.sym.s_cond), cond_clauses);
          Value handler_lambda = list(sym(vm.sym.s_lambda), list(var), cond_form);
          Value thunk_lambda   = call_form(sym(vm.sym.s_lambda),
                                           cons(Value::nil(), body));
          Value guard_call     = list(sym(vm.sym.s_guard_impl),
                                      handler_lambda, thunk_lambda);
          compile_expr(guard_call, chunk, is_tail);
          return;
        }

        // (let ((v e)...) body...) or (let [v e ...] body...)
        if (op_name == "let") {
          Value bindings = Heap::car(rest);
          Value let_body = Heap::cdr(rest);

          // Check if named let: (let loop ((v e)...) body...) or (let loop [v e
          // ...] body...)
          if (bindings.is_symbol()) {
            Value loop_name = bindings;
            bindings = Heap::car(let_body);
            let_body = Heap::cdr(let_body);

            auto pairs = parse_bindings(bindings);
            std::vector<Value> params;
            std::vector<Value> inits;
            for (const auto &p : pairs) {
              params.push_back(p.var);
              inits.push_back(p.val);
            }

            Value param_list = Value::nil();
            for (auto it = params.rbegin(); it != params.rend(); ++it) {
              param_list = vm.heap.cons(*it, param_list);
            }

            uint32_t arity = 0;
            std::vector<UpvalueDesc> upvals;
            std::string loop_str = vm.get_symbol_name(loop_name.as_symbol_id());
            ObjClosure *closure =
                compile_function(param_list, let_body, arity, upvals, loop_str);

            // Push callee closure and initial arguments, then call
            emit_closure(closure, upvals, chunk);
            for (Value init_expr : inits) {
              compile_expr(init_expr, chunk, false);
            }
            chunk.code.push_back(is_tail ? OP_TAIL_CALL : OP_CALL);
            chunk.code.push_back(static_cast<uint8_t>(inits.size()));
            return;
          }

          // ---- Fast path: bind into the enclosing frame, body inline ----
          //
          // The fallback below compiles `let` as ((lambda (v...) body) e...),
          // which allocates an ObjClosure on every ENTRY to the scope —
          // measured at 1 object per `let`, and since let*/letrec/do/case/when
          // all desugar through here, it was the single largest allocation
          // source in ordinary Scheme code (the wrangle demo was burning
          // ~4,600 objects/frame, 99.2% of it immediate garbage).
          //
          // Instead the bindings can occupy local slots in the frame we are
          // already in, with the body compiled inline: no closure, no call.
          // The body's tail position becomes the enclosing function's, which
          // is strictly better for TCO than bouncing through OP_TAIL_CALL.
          //
          // Capture remains correct thanks to OP_INIT_LOCAL's unconditional
          // store. If an inner lambda captures one of these bindings,
          // OP_CLOSURE boxes the slot in place; re-entering the scope
          // overwrites the slot with a raw value and leaves that box owned
          // by the closure holding it — a fresh binding per entry, which is
          // what `let` means. (OP_SET_LOCAL would instead assign *through*
          // the stale box and corrupt the earlier binding.)
          //
          // Bail-out: a body with leading internal (define ...) forms needs
          // the fresh letrec scope compile_function establishes, so those
          // keep the old path rather than leaking definitions into the
          // enclosing scope.
          if (!VXS_INLINE_LET_DISABLED && !body_starts_with_define(let_body)) {
            auto inline_pairs = parse_bindings(bindings);

            // 1. Initializers all evaluate in the OUTER scope — the new
            //    names must not be visible to them (parallel let).
            for (const auto &pr : inline_pairs) {
              compile_expr(pr.val, chunk, false);
            }

            // 2. Now the names come into scope, and the values are popped
            //    back off in reverse (the operand stack is LIFO).
            size_t saved_locals = locals.size();
            std::vector<int> slots;
            slots.reserve(inline_pairs.size());
            for (const auto &pr : inline_pairs) {
              slots.push_back(add_local(vm.get_symbol_name(pr.var.as_symbol_id())));
            }
            for (size_t i = slots.size(); i-- > 0;) {
              emit_op(OP_INIT_LOCAL, static_cast<uint16_t>(slots[i]), chunk);
            }

            // 3. Body, inline. Its last form inherits OUR tail position.
            Value bexp = let_body;
            if (bexp.is_nil()) {
              chunk.code.push_back(OP_UNSPECIFIED);
            } else {
              while (Heap::is_cons(bexp)) {
                bool is_last = Heap::cdr(bexp).is_nil();
                compile_expr(Heap::car(bexp), chunk, is_last && is_tail);
                if (!is_last) chunk.code.push_back(OP_POP);
                bexp = Heap::cdr(bexp);
              }
            }

            // Names go out of scope; max_locals keeps the high-water mark,
            // so sibling scopes reuse these slots.
            locals.resize(saved_locals);
            return;
          }

          // Standard parallel let: ((lambda (v1 v2 ...) body...) e1 e2 ...)
          // Always routed through compile_function (even with zero bindings)
          // so internal (define ...) forms in the body get their own fresh
          // scope via the letrec desugaring there, rather than leaking into
          // the enclosing scope.
          auto pairs = parse_bindings(bindings);
          std::vector<Value> params;
          std::vector<Value> inits;
          for (const auto &p : pairs) {
            params.push_back(p.var);
            inits.push_back(p.val);
          }

          Value param_list = Value::nil();
          for (auto it = params.rbegin(); it != params.rend(); ++it) {
            param_list = vm.heap.cons(*it, param_list);
          }

          uint32_t arity = 0;
          std::vector<UpvalueDesc> upvals;
          ObjClosure *closure =
              compile_function(param_list, let_body, arity, upvals);
          emit_closure(closure, upvals, chunk);
          for (Value init_expr : inits) {
            compile_expr(init_expr, chunk, false);
          }
          chunk.code.push_back(is_tail ? OP_TAIL_CALL : OP_CALL);
          chunk.code.push_back(static_cast<uint8_t>(inits.size()));
          return;
        }

        // (letrec ((v1 e1) (v2 e2)...) body...) or (letrec [v1 e1 ...] body...)
        if (op_name == "letrec") {
          Value bindings = Heap::car(rest);
          Value body = Heap::cdr(rest);
          // Always desugar to `let` (even with zero bindings) so the body
          // gets its own fresh scope for internal defines — see the `let`
          // comment above.
          auto pairs = parse_bindings(bindings);

          Value let_sym = Value::from_symbol_id(vm.intern("let"));
          Value set_sym = Value::from_symbol_id(vm.intern("set!"));
          Value void_sym = Value::from_symbol_id(vm.intern("void"));
          Value void_call = vm.heap.cons(void_sym, Value::nil());

          // Build (let ((v1 (void)) (v2 (void))...) (set! v1 e1) (set! v2
          // e2)... body...)
          std::vector<Value> init_bindings;
          std::vector<Value> set_exprs;
          for (const auto &p : pairs) {
            init_bindings.push_back(
                vm.heap.cons(p.var, vm.heap.cons(void_call, Value::nil())));
            set_exprs.push_back(vm.heap.cons(
                set_sym,
                vm.heap.cons(p.var, vm.heap.cons(p.val, Value::nil()))));
          }

          Value binding_list = Value::nil();
          for (auto it = init_bindings.rbegin(); it != init_bindings.rend();
               ++it) {
            binding_list = vm.heap.cons(*it, binding_list);
          }

          // Wrap the original body in its own (let () ...) rather than
          // splicing it after the set! initializers directly: the set!
          // forms would otherwise occupy the "leading" position that
          // compile_function scans for internal defines, so a (define ...)
          // at the head of the user's body would stop being recognized as
          // one and would fall through to a global define instead of
          // shadowing within its own nested scope.
          std::vector<Value> full_body = set_exprs;
          full_body.push_back(
              vm.heap.cons(let_sym, vm.heap.cons(Value::nil(), body)));

          Value body_list = Value::nil();
          for (auto it = full_body.rbegin(); it != full_body.rend(); ++it) {
            body_list = vm.heap.cons(*it, body_list);
          }

          Value desugared =
              vm.heap.cons(let_sym, vm.heap.cons(binding_list, body_list));
          compile_expr(desugared, chunk, is_tail);
          return;
        }

        // (when test body...)
        if (op_name == "when") {
          // (when test body...) => (if test (begin body...) #<unspecified>)
          Value desugared = list(sym(vm.sym.s_if), Heap::car(rest),
                                 call_form(sym(vm.sym.s_begin), Heap::cdr(rest)),
                                 Value::unspecified());
          compile_expr(desugared, chunk, is_tail);
          return;
        }

        // (unless test body...)
        if (op_name == "unless") {
          // (unless test body...) => (if test #<unspecified> (begin body...))
          Value desugared = list(sym(vm.sym.s_if), Heap::car(rest),
                                 Value::unspecified(),
                                 call_form(sym(vm.sym.s_begin), Heap::cdr(rest)));
          compile_expr(desugared, chunk, is_tail);
          return;
        }

        // (cond (test1 e1...) (else ee...) ...)
        if (op_name == "cond") {
          Value clauses = rest;
          if (clauses.is_nil()) {
            chunk.code.push_back(OP_UNSPECIFIED);
            return;
          }
          std::vector<Value> clause_vec;
          while (Heap::is_cons(clauses)) {
            clause_vec.push_back(Heap::car(clauses));
            clauses = Heap::cdr(clauses);
          }

          // Built back to front, each clause wrapping the rest:
          //   (test body...)   => (if test (begin body...) <rest>)
          //   (test => recip)  => (let ((t test)) (if t (recip t) <rest>))
          //   (else body...)   => (begin body...)          [terminates]
          Value current = Value::unspecified();

          for (auto it = clause_vec.rbegin(); it != clause_vec.rend(); ++it) {
            Value clause = *it;
            Value test = Heap::car(clause);
            Value body = Heap::cdr(clause);

            // (test => recipient) — exactly one form after the arrow.
            Value recipient = Value::nil();
            if (!body.is_nil() && Heap::car(body).is_symbol() &&
                Heap::car(body).as_symbol_id() == vm.sym.s_arrow &&
                Heap::is_cons(Heap::cdr(body)) &&
                Heap::cdr(Heap::cdr(body)) == Value::nil()) {
              recipient = Heap::car(Heap::cdr(body));
              body = Heap::cdr(Heap::cdr(body));
            }

            if (!recipient.is_nil()) {
              // The test's value is needed twice, so bind it once — and
              // gensym the name, since the recipient may close over
              // anything the user has in scope.
              Value t = sym(vm.intern("$test__" +
                                      std::to_string(vm.next_gensym_id++)));
              current = list(sym(vm.sym.s_let),
                             list(list(t, test)),
                             list(sym(vm.sym.s_if), t,
                                  list(recipient, t),
                                  current));
            } else {
              Value body_seq = call_form(sym(vm.sym.s_begin), body);
              if (test.is_symbol() && test.as_symbol_id() == vm.sym.s_else) {
                current = body_seq;
              } else {
                current = list(sym(vm.sym.s_if), test, body_seq, current);
              }
            }
          }

          compile_expr(current, chunk, is_tail);
          return;
        }

        // (case key ((d1 d2...) body...) ... (else body...))
        if (op_name == "case") {
          Value key_expr = Heap::car(rest);
          Value clauses = Heap::cdr(rest);

          // (case key ((d...) body...) ... (else body...))
          //   =>
          // (let ((k key))
          //   (cond ((memv k '(d...)) body...) ... (else body...)))
          //
          // k is GENSYMED. It used to be a fixed `$case_key`, which meant
          // a user variable of that name was shadowed by the expansion —
          //   (let (($case_key 99)) (case 1 ((1) $case_key)))  => 1, not 99
          // The `=>` branch of cond directly above always gensymed; case
          // simply did not.
          Value k = sym(vm.intern("$case_key__" +
                                  std::to_string(vm.next_gensym_id++)));

          std::vector<Value> cond_clauses;
          for (Value cur = clauses; Heap::is_cons(cur); cur = Heap::cdr(cur)) {
            Value clause = Heap::car(cur);
            if (!Heap::is_cons(clause)) continue;
            Value datums = Heap::car(clause);
            Value body = Heap::cdr(clause);
            if (datums.is_symbol() && datums.as_symbol_id() == vm.sym.s_else) {
              cond_clauses.push_back(clause);
            } else {
              Value test = list(sym(vm.sym.s_memv), k,
                                list(sym(vm.sym.s_quote), datums));
              cond_clauses.push_back(call_form(test, body));
            }
          }

          Value cond_clause_list = Value::nil();
          for (auto it = cond_clauses.rbegin(); it != cond_clauses.rend(); ++it) {
            cond_clause_list = cons(*it, cond_clause_list);
          }

          Value desugared = list(sym(vm.sym.s_let),
                                 list(list(k, key_expr)),
                                 call_form(sym(vm.sym.s_cond), cond_clause_list));
          compile_expr(desugared, chunk, is_tail);
          return;
        }

        // (let* ((v1 e1) (v2 e2)...) body...) or (let* [v1 e1 ...] body...)
        if (op_name == "let*") {
          Value bindings = Heap::car(rest);
          Value body = Heap::cdr(rest);
          Value let_sym = Value::from_symbol_id(vm.intern("let"));

          auto pairs = parse_bindings(bindings);
          if (pairs.empty()) {
            // (let () body...) — routes through the `let` handler above,
            // which opens a fresh scope for internal defines.
            compile_expr(vm.heap.cons(let_sym, vm.heap.cons(Value::nil(), body)),
                        chunk, is_tail);
            return;
          }

          Value current = vm.heap.cons(
              let_sym,
              vm.heap.cons(
                  vm.heap.cons(vm.heap.cons(pairs.back().var,
                                            vm.heap.cons(pairs.back().val,
                                                         Value::nil())),
                               Value::nil()),
                  body));

          for (int i = static_cast<int>(pairs.size()) - 2; i >= 0; --i) {
            current = vm.heap.cons(
                let_sym,
                vm.heap.cons(
                    vm.heap.cons(
                        vm.heap.cons(pairs[i].var,
                                     vm.heap.cons(pairs[i].val, Value::nil())),
                        Value::nil()),
                    vm.heap.cons(current, Value::nil())));
          }

          compile_expr(current, chunk, is_tail);
          return;
        }

        // (let-values (((a b) e1) ((c) e2) ...) body...) — like let*
        // but each binding clause's formal list is bound to the multiple
        // values (call-with-values) production of its expression.
        // let-values is "parallel": e1/e2/... are all evaluated in the
        // OUTER scope, none seeing each other's bindings (only body
        // does) — matching plain `let` vs `let*`. Desugars to nested
        // call-with-values calls; let-values additionally routes each
        // clause through gensym'd formals first (see gensym_formals)
        // so an inner producer expression can't accidentally resolve to
        // an outer clause's real binding name, then rebinds the real
        // names via one flat `let` wrapping the body.
        if (op_name == "let-values" || op_name == "let*-values") {
          bool parallel = (op_name == "let-values");
          Value clauses = Heap::car(rest);
          Value body = Heap::cdr(rest);

          Value let_sym = Value::from_symbol_id(vm.intern("let"));
          Value lambda_sym = Value::from_symbol_id(vm.intern("lambda"));
          Value cwv_sym = Value::from_symbol_id(vm.intern("call-with-values"));

          std::vector<std::pair<Value, Value>> clause_list; // (formals, expr)
          Value cur = clauses;
          while (Heap::is_cons(cur)) {
            Value clause = Heap::car(cur);
            clause_list.push_back({Heap::car(clause), Heap::car(Heap::cdr(clause))});
            cur = Heap::cdr(cur);
          }

          if (clause_list.empty()) {
            // (let-values () body...) — routes through `let` for its
            // internal-define handling, same as let*'s empty case.
            compile_expr(vm.heap.cons(let_sym, vm.heap.cons(Value::nil(), body)),
                        chunk, is_tail);
            return;
          }

          std::vector<Value> use_formals;
          std::vector<std::pair<Value, Value>> real_temp_pairs;
          for (auto &cl : clause_list) {
            use_formals.push_back(parallel ? gensym_formals(cl.first, real_temp_pairs) : cl.first);
          }

          Value inner_body = body;
          if (parallel) {
            Value let_bindings = Value::nil();
            for (auto it = real_temp_pairs.rbegin(); it != real_temp_pairs.rend(); ++it) {
              Value pair_form = vm.heap.cons(it->first, vm.heap.cons(it->second, Value::nil()));
              let_bindings = vm.heap.cons(pair_form, let_bindings);
            }
            Value let_form = vm.heap.cons(let_sym, vm.heap.cons(let_bindings, body));
            inner_body = vm.heap.cons(let_form, Value::nil());
          }

          Value current_body = inner_body;
          for (int i = static_cast<int>(clause_list.size()) - 1; i >= 0; --i) {
            Value producer_lambda = vm.heap.cons(
                lambda_sym, vm.heap.cons(Value::nil(),
                                         vm.heap.cons(clause_list[i].second, Value::nil())));
            Value consumer_lambda = vm.heap.cons(lambda_sym, vm.heap.cons(use_formals[i], current_body));
            Value cwv_call = vm.heap.cons(
                cwv_sym, vm.heap.cons(producer_lambda, vm.heap.cons(consumer_lambda, Value::nil())));
            current_body = vm.heap.cons(cwv_call, Value::nil());
          }

          compile_expr(Heap::car(current_body), chunk, is_tail);
          return;
        }

        // (do ((var init [step]) ...) (test expr...) cmd...) or (do [var init
        // [step] ...] ...)
        if (op_name == "do") {
          Value var_clauses = Heap::car(rest);
          Value test_clause = Heap::car(Heap::cdr(rest));
          Value commands = Heap::cdr(Heap::cdr(rest));

          if (Heap::is_cons(test_clause) &&
              Heap::car(test_clause).is_symbol() &&
              vm.get_symbol_name(Heap::car(test_clause).as_symbol_id()) ==
                  "%bracket-vector") {
            test_clause = Heap::cdr(test_clause);
          }

          Value test_expr = Heap::car(test_clause);
          Value result_exprs = Heap::cdr(test_clause);

          std::vector<Value> bindings;
          std::vector<Value> step_args;

          if (Heap::is_cons(var_clauses) &&
              Heap::car(var_clauses).is_symbol() &&
              vm.get_symbol_name(Heap::car(var_clauses).as_symbol_id()) ==
                  "%bracket-vector") {
            Value elems = Heap::cdr(var_clauses);
            while (Heap::is_cons(elems)) {
              Value v_name = Heap::car(elems);
              elems = Heap::cdr(elems);
              Value v_init = Heap::is_cons(elems) ? Heap::car(elems)
                                                  : Value::unspecified();
              if (Heap::is_cons(elems))
                elems = Heap::cdr(elems);
              Value v_step = v_name;
              if (Heap::is_cons(elems) && !Heap::car(elems).is_symbol()) {
                v_step = Heap::car(elems);
                elems = Heap::cdr(elems);
              }
              bindings.push_back(
                  vm.heap.cons(v_name, vm.heap.cons(v_init, Value::nil())));
              step_args.push_back(v_step);
            }
          } else {
            Value vc = var_clauses;
            while (Heap::is_cons(vc)) {
              Value item = Heap::car(vc);
              Value v_name = Heap::car(item);
              Value v_init = Heap::car(Heap::cdr(item));
              Value v_step = Heap::is_cons(Heap::cdr(Heap::cdr(item)))
                                 ? Heap::car(Heap::cdr(Heap::cdr(item)))
                                 : v_name;

              bindings.push_back(
                  vm.heap.cons(v_name, vm.heap.cons(v_init, Value::nil())));
              step_args.push_back(v_step);
              vc = Heap::cdr(vc);
            }
          }

          Value binding_list = Value::nil();
          for (auto it = bindings.rbegin(); it != bindings.rend(); ++it) {
            binding_list = vm.heap.cons(*it, binding_list);
          }

          Value step_list = Value::nil();
          for (auto it = step_args.rbegin(); it != step_args.rend(); ++it) {
            step_list = vm.heap.cons(*it, step_list);
          }

          // (do ((v init step)...) (test result...) command...)
          //   =>
          // (let loop ((v init)...)
          //   (if test (begin result...)
          //            (begin command... (loop step...))))
          Value loop_sym = sym(vm.sym.s_loop);

          Value loop_call   = call_form(loop_sym, step_list);
          Value else_branch = call_form(sym(vm.sym.s_begin),
                                        append_last(commands, loop_call));
          Value then_branch = call_form(sym(vm.sym.s_begin), result_exprs);
          Value if_form     = list(sym(vm.sym.s_if), test_expr,
                                   then_branch, else_branch);
          Value desugared   = list(sym(vm.sym.s_let), loop_sym,
                                   binding_list, if_form);

          compile_expr(desugared, chunk, is_tail);
          return;
        }

        // (and e1 e2 ...)
        if (op_name == "and") {
          if (rest.is_nil()) {
            chunk.code.push_back(OP_TRUE);
            return;
          }
          std::vector<Value> exprs;
          Value cur = rest;
          while (Heap::is_cons(cur)) {
            exprs.push_back(Heap::car(cur));
            cur = Heap::cdr(cur);
          }

          // (and a b c) => (if a (if b c #f) #f). No temporary needed:
          // and discards each test's value, so nothing must be re-read.
          Value current = exprs.back();
          for (int i = static_cast<int>(exprs.size()) - 2; i >= 0; --i) {
            current = list(sym(vm.sym.s_if), exprs[i], current,
                           Value::boolean_false());
          }
          compile_expr(current, chunk, is_tail);
          return;
        }

        // (or e1 e2 ...)
        if (op_name == "or") {
          if (rest.is_nil()) {
            chunk.code.push_back(OP_FALSE);
            return;
          }
          std::vector<Value> exprs;
          Value cur = rest;
          while (Heap::is_cons(cur)) {
            exprs.push_back(Heap::car(cur));
            cur = Heap::cdr(cur);
          }

          // (or a b) => (let ((t a)) (if t t b)) — a temporary IS needed
          // here, because or returns the test's own value and must not
          // evaluate it twice.
          //
          // t is GENSYMED. It used to be "$or_" + the loop index, so the
          // name repeated across every `or` in the program and shadowed
          // any user variable that happened to match:
          //   (let (($or_0 42)) (or #f $or_0))   => #f, not 42
          Value current = exprs.back();
          for (int i = static_cast<int>(exprs.size()) - 2; i >= 0; --i) {
            Value t = sym(vm.intern("$or__" +
                                    std::to_string(vm.next_gensym_id++)));
            current = list(sym(vm.sym.s_let),
                           list(list(t, exprs[i])),
                           list(sym(vm.sym.s_if), t, t, current));
          }
          compile_expr(current, chunk, is_tail);
          return;
        }

        // (quasiquote expr)
        if (op_name == "quasiquote") {
          std::unordered_map<std::string, Value> gensym_map;
          Value expanded = expand_quasiquote(Heap::car(rest), gensym_map, 1);
          compile_expr(expanded, chunk, is_tail);
          return;
        }

        // (defmacro name (args...) body...) or (defmacro (name args...)
        // body...)
        if (op_name == "defmacro" || op_name == "define-macro") {
          Value name_val;
          Value params_val;
          Value body_val;
          Value target = Heap::car(rest);
          if (target.is_symbol()) {
            name_val = target;
            params_val = Heap::car(Heap::cdr(rest));
            body_val = Heap::cdr(Heap::cdr(rest));
          } else if (Heap::is_cons(target)) {
            name_val = Heap::car(target);
            params_val = Heap::cdr(target);
            body_val = Heap::cdr(rest);
          } else {
            chunk.code.push_back(OP_UNSPECIFIED);
            return;
          }
          std::string macro_name = vm.get_symbol_name(name_val.as_symbol_id());
          uint32_t arity = 0;
          std::vector<UpvalueDesc> upvals;
          ObjClosure *transformer =
              compile_function(params_val, body_val, arity, upvals, macro_name);
          vm.macros[macro_name] = transformer;
          chunk.code.push_back(OP_UNSPECIFIED);
          return;
        }

        // Check for user-defined macro in VM::macros
        auto macro_it = vm.macros.find(op_name);
        if (macro_it != vm.macros.end()) {
          ObjClosure *transformer = macro_it->second;
          std::vector<Value> raw_args;
          Value cur_m = rest;
          while (Heap::is_cons(cur_m)) {
            raw_args.push_back(Heap::car(cur_m));
            cur_m = Heap::cdr(cur_m);
          }
          Value expanded = vm.call_closure(transformer, raw_args);
          if (expanded.is_unspecified() && !vm.last_error.empty()) {
            std::cerr << "[Macro Error in " << op_name << "] " << vm.last_error
                      << std::endl;
          }
          compile_expr(expanded, chunk, is_tail);
          return;
        }
      }

      // Standard Function Application: (callee arg1 arg2 ...)
      compile_expr(head, chunk, false);
      uint8_t argc = 0;
      Value cur_arg = rest;
      while (Heap::is_cons(cur_arg)) {
        compile_expr(Heap::car(cur_arg), chunk, false);
        ++argc;
        cur_arg = Heap::cdr(cur_arg);
      }

      chunk.code.push_back(is_tail ? OP_TAIL_CALL : OP_CALL);
      chunk.code.push_back(argc);
    }
  }

  uint16_t add_constant(Value v, BytecodeChunk &chunk) {
    chunk.constants.push_back(v);
    return static_cast<uint16_t>(chunk.constants.size() - 1);
  }

  void emit_constant(Value v, BytecodeChunk &chunk) {
    if (v.is_nil()) {
      chunk.code.push_back(OP_NIL);
    } else if (v.is_bool() && v.as_bool()) {
      chunk.code.push_back(OP_TRUE);
    } else if (v.is_bool() && !v.as_bool()) {
      chunk.code.push_back(OP_FALSE);
    } else if (v.is_unspecified()) {
      chunk.code.push_back(OP_UNSPECIFIED);
    } else {
      uint16_t const_ix = add_constant(v, chunk);
      emit_op(OP_CONST, const_ix, chunk);
    }
  }

  void emit_closure(ObjClosure *proto, const std::vector<UpvalueDesc> &upvals,
                    BytecodeChunk &chunk) {
    uint16_t const_ix = add_constant(Value::from_ptr(proto), chunk);
    chunk.code.push_back(OP_CLOSURE);
    chunk.code.push_back(static_cast<uint8_t>((const_ix >> 8) & 0xFF));
    chunk.code.push_back(static_cast<uint8_t>(const_ix & 0xFF));
    chunk.code.push_back(static_cast<uint8_t>(upvals.size()));
    for (const auto &uv : upvals) {
      chunk.code.push_back(uv.is_local ? 1 : 0);
      chunk.code.push_back(uv.index);
    }
  }

  void emit_op(Opcode op, uint16_t operand, BytecodeChunk &chunk) {
    chunk.code.push_back(op);
    chunk.code.push_back(static_cast<uint8_t>((operand >> 8) & 0xFF));
    chunk.code.push_back(static_cast<uint8_t>(operand & 0xFF));
  }

  void patch_jump(size_t offset_index, uint16_t jump_amount,
                  BytecodeChunk &chunk) {
    chunk.code[offset_index] = static_cast<uint8_t>((jump_amount >> 8) & 0xFF);
    chunk.code[offset_index + 1] = static_cast<uint8_t>(jump_amount & 0xFF);
  }

  // True if the body opens with an internal (define ...). Such bodies need
  // the letrec scope compile_function builds, so the inline `let` path
  // declines them. Only the leading run matters — that is the same window
  // compile_function scans.
  bool body_starts_with_define(Value body) {
    if (!Heap::is_cons(body)) return false;
    Value form = Heap::car(body);
    if (!Heap::is_cons(form)) return false;
    Value head = Heap::car(form);
    if (!head.is_symbol()) return false;
    return vm.get_symbol_name(head.as_symbol_id()) == "define";
  }

  int add_local(const std::string &name) {
    locals.push_back({name, scope_depth, false});
    if (locals.size() > max_locals)
      max_locals = locals.size();
    return static_cast<int>(locals.size() - 1);
  }

  int resolve_local(const std::string &name) {
    for (int i = static_cast<int>(locals.size()) - 1; i >= 0; --i) {
      if (locals[i].name == name)
        return i;
    }
    return -1;
  }

  int resolve_upvalue(const std::string &name) {
    if (!parent)
      return -1;

    int local = parent->resolve_local(name);
    if (local != -1) {
      parent->locals[local].is_captured = true;
      return add_upvalue(static_cast<uint8_t>(local), true);
    }

    int upval = parent->resolve_upvalue(name);
    if (upval != -1) {
      return add_upvalue(static_cast<uint8_t>(upval), false);
    }

    return -1;
  }

  int add_upvalue(uint8_t index, bool is_local) {
    for (size_t i = 0; i < upvalues.size(); ++i) {
      if (upvalues[i].index == index && upvalues[i].is_local == is_local) {
        return static_cast<int>(i);
      }
    }
    upvalues.push_back({index, is_local});
    return static_cast<int>(upvalues.size() - 1);
  }

  // Produces a same-shaped copy of `formals` (a lambda-style formal
  // list — proper, dotted, or a bare symbol) with every name replaced by
  // a fresh gensym, recording each (real-name, temp-name) pair in
  // `out_pairs`. Used by let-values (see below) to evaluate each
  // binding clause's producer expression in a scope isolated from every
  // OTHER clause's bindings — true "parallel" let-values semantics —
  // by binding through anonymous temporaries first and only introducing
  // the real names afterward, in one flat `let` around the body.
  Value gensym_formals(Value formals, std::vector<std::pair<Value, Value>> &out_pairs) {
    if (formals.is_symbol()) {
      Value temp = Value::from_symbol_id(vm.intern("$lv__" + std::to_string(vm.next_gensym_id++)));
      out_pairs.push_back({formals, temp});
      return temp;
    }
    if (Heap::is_cons(formals)) {
      Value temp_head = Value::from_symbol_id(vm.intern("$lv__" + std::to_string(vm.next_gensym_id++)));
      out_pairs.push_back({Heap::car(formals), temp_head});
      return vm.heap.cons(temp_head, gensym_formals(Heap::cdr(formals), out_pairs));
    }
    return formals; // nil tail of a proper list
  }

  struct BindingPair {
    Value var;
    Value val;
  };

  std::vector<BindingPair> parse_bindings(Value bindings) {
    std::vector<BindingPair> result;
    if (bindings.is_nil())
      return result;

    // Check if direct ObjVector: [var1 val1 var2 val2 ...]
    if (Heap::is_vector(bindings)) {
      ObjVector *vec = bindings.as_ptr<ObjVector>();
      for (uint32_t i = 0; i < vec->size; i += 2) {
        Value var = vec->get(i);
        Value val =
            (i + 1 < vec->size) ? vec->get(i + 1) : Value::unspecified();
        result.push_back({var, val});
      }
      return result;
    }

    // Check if bracketed vector AST: (%bracket-vector var1 val1 var2 val2 ...)
    if (Heap::is_cons(bindings) && Heap::car(bindings).is_symbol() &&
        vm.get_symbol_name(Heap::car(bindings).as_symbol_id()) == "%bracket-vector") {
      Value elems = Heap::cdr(bindings);
      while (Heap::is_cons(elems)) {
        Value var = Heap::car(elems);
        elems = Heap::cdr(elems);
        Value val =
            Heap::is_cons(elems) ? Heap::car(elems) : Value::unspecified();
        if (Heap::is_cons(elems))
          elems = Heap::cdr(elems);
        result.push_back({var, val});
      }
      return result;
    }

    // Classic list of pairs: ((var1 val1) (var2 val2) ...)
    Value b = bindings;
    while (Heap::is_cons(b)) {
      Value pair = Heap::car(b);
      if (Heap::is_cons(pair)) {
        Value var = Heap::car(pair);
        Value val = Heap::is_cons(Heap::cdr(pair)) ? Heap::car(Heap::cdr(pair))
                                                   : Value::unspecified();
        result.push_back({var, val});
      }
      b = Heap::cdr(b);
    }
    return result;
  }

  // Shared by both quasiquoted (vector ...) forms and raw [...] vector
  // literals: builds (list->vector (append (list e1) (list e2) ... spliced))
  Value expand_quasiquote_vector_elems(
      const std::vector<Value> &elems,
      std::unordered_map<std::string, Value> &gensym_map, int depth) {
    std::vector<Value> v_parts;
    for (Value elem : elems) {
      if (Heap::is_cons(elem) && Heap::car(elem).is_symbol() &&
          vm.get_symbol_name(Heap::car(elem).as_symbol_id()) ==
              "unquote-splicing" &&
          depth == 1) {
        v_parts.push_back(Heap::car(Heap::cdr(elem)));
      } else {
        Value expanded_elem = expand_quasiquote(elem, gensym_map, depth);
        v_parts.push_back(
            vm.heap.cons(Value::from_symbol_id(vm.intern("list")),
                         vm.heap.cons(expanded_elem, Value::nil())));
      }
    }
    Value append_sym = Value::from_symbol_id(vm.intern("append"));
    Value list_to_vec_sym = Value::from_symbol_id(vm.intern("list->vector"));
    Value v_res_list = Value::nil();
    for (auto it = v_parts.rbegin(); it != v_parts.rend(); ++it) {
      v_res_list = vm.heap.cons(*it, v_res_list);
    }
    return vm.heap.cons(
        list_to_vec_sym,
        vm.heap.cons(vm.heap.cons(append_sym, v_res_list), Value::nil()));
  }

  Value expand_quasiquote(Value form,
                          std::unordered_map<std::string, Value> &gensym_map,
                          int depth = 1) {
    if (Heap::is_vector(form)) {
      ObjVector *vec = form.as_ptr<ObjVector>();
      std::vector<Value> elems(vec->data, vec->data + vec->size);
      return expand_quasiquote_vector_elems(elems, gensym_map, depth);
    }
    if (!Heap::is_cons(form)) {
      if (form.is_symbol()) {
        std::string sym_name = vm.get_symbol_name(form.as_symbol_id());
        if (sym_name.size() > 1 && sym_name.back() == '#') {
          auto it = gensym_map.find(sym_name);
          if (it == gensym_map.end()) {
            std::string base = sym_name.substr(0, sym_name.size() - 1);
            std::string generated =
                base + "__" + std::to_string(vm.next_gensym_id++) + "__auto__";
            Value gen_sym = Value::from_symbol_id(vm.intern(generated));
            gensym_map[sym_name] = gen_sym;
            return vm.heap.cons(Value::from_symbol_id(vm.intern("quote")),
                                vm.heap.cons(gen_sym, Value::nil()));
          } else {
            return vm.heap.cons(Value::from_symbol_id(vm.intern("quote")),
                                vm.heap.cons(it->second, Value::nil()));
          }
        }
      }
      return vm.heap.cons(Value::from_symbol_id(vm.intern("quote")),
                          vm.heap.cons(form, Value::nil()));
    }

    Value head = Heap::car(form);
    Value rest = Heap::cdr(form);

    if (head.is_symbol()) {
      std::string sym = vm.get_symbol_name(head.as_symbol_id());
      if (sym == "quasiquote") {
        return vm.heap.cons(
            Value::from_symbol_id(vm.intern("list")),
            vm.heap.cons(vm.heap.cons(Value::from_symbol_id(vm.intern("quote")),
                                      vm.heap.cons(head, Value::nil())),
                         vm.heap.cons(expand_quasiquote(Heap::car(rest),
                                                        gensym_map, depth + 1),
                                      Value::nil())));
      }
      if (sym == "unquote") {
        if (depth == 1) {
          return Heap::car(rest);
        } else {
          return vm.heap.cons(
              Value::from_symbol_id(vm.intern("list")),
              vm.heap.cons(
                  vm.heap.cons(Value::from_symbol_id(vm.intern("quote")),
                               vm.heap.cons(head, Value::nil())),
                  vm.heap.cons(
                      expand_quasiquote(Heap::car(rest), gensym_map, depth - 1),
                      Value::nil())));
        }
      }
      if (sym == "%bracket-vector") {
        std::vector<Value> elems;
        Value cur_v = rest;
        while (Heap::is_cons(cur_v)) {
          elems.push_back(Heap::car(cur_v));
          cur_v = Heap::cdr(cur_v);
        }
        return expand_quasiquote_vector_elems(elems, gensym_map, depth);
      }
    }

    // List elements: check for unquote-splicing
    std::vector<Value> parts;
    Value cur = form;
    while (Heap::is_cons(cur)) {
      // Dotted-tail unquote: `(a . ,b) reads as the cons chain (a unquote
      // b), indistinguishable from a proper 3-element list at this point —
      // per R4RS both forms mean the same thing. Stop consuming `cur` as an
      // ordinary spine element so the tail handling below can hand
      // (unquote b) to expand_quasiquote, which recognizes it directly.
      if (Heap::car(cur).is_symbol() &&
          vm.get_symbol_name(Heap::car(cur).as_symbol_id()) == "unquote" &&
          Heap::is_cons(Heap::cdr(cur))) {
        break;
      }
      Value elem = Heap::car(cur);
      if (Heap::is_cons(elem) && Heap::car(elem).is_symbol() &&
          vm.get_symbol_name(Heap::car(elem).as_symbol_id()) ==
              "unquote-splicing" &&
          depth == 1) {
        parts.push_back(Heap::car(Heap::cdr(elem)));
      } else {
        Value expanded_elem = expand_quasiquote(elem, gensym_map, depth);
        parts.push_back(
            vm.heap.cons(Value::from_symbol_id(vm.intern("list")),
                         vm.heap.cons(expanded_elem, Value::nil())));
      }
      cur = Heap::cdr(cur);
    }

    if (!cur.is_nil()) {
      Value expanded_tail = expand_quasiquote(cur, gensym_map, depth);
      parts.push_back(expanded_tail);
    }

    Value append_sym = Value::from_symbol_id(vm.intern("append"));
    Value res_list = Value::nil();
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
      res_list = vm.heap.cons(*it, res_list);
    }
    return vm.heap.cons(append_sym, res_list);
  }

  VM &vm;
  Compiler *parent;
  std::vector<Local> locals;
  std::vector<UpvalueDesc> upvalues;
  int scope_depth;
  size_t max_locals;
};

} // namespace vxs
