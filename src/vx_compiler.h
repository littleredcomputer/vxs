#pragma once

#include "vx_value.h"
#include "vx_heap.h"
#include "vx_vm.h"
#include "vx_reader.h"
#include <vector>
#include <string>
#include <stdexcept>

namespace vxs {

struct Local {
  std::string name;
  int depth;
  bool is_captured;
};

struct UpvalueDesc {
  uint8_t index;
  bool is_local;
};

class Compiler {
public:
  Compiler(VM &vm, Compiler *parent = nullptr)
      : vm(vm), parent(parent), scope_depth(0), max_locals(1) {}

  ObjClosure *compile_top_level(Value form) {
    BytecodeChunk *chunk = new BytecodeChunk();
    add_local("<top-level>");
    compile_expr(form, *chunk, false);
    chunk->code.push_back(OP_RETURN);
    return vm.heap.allocate<ObjClosure>(chunk, 0, false, 0, static_cast<uint32_t>(max_locals));
  }

  ObjClosure *compile_function(Value params, Value body, uint32_t &out_arity, std::vector<UpvalueDesc> &out_upvals, const std::string &self_name = "") {
    BytecodeChunk *chunk = new BytecodeChunk();
    Compiler fn_compiler(vm, this);
    fn_compiler.scope_depth = 1;

    // Slot 0 is the callee closure itself (for self-recursion like named let)
    fn_compiler.add_local(self_name.empty() ? "<callee>" : self_name);

    // Register parameter locals (slots 1..N)
    uint32_t arity = 0;
    Value p = params;
    while (Heap::is_cons(p)) {
      Value sym_val = Heap::car(p);
      assert(sym_val.is_symbol());
      std::string name = vm.get_symbol_name(sym_val.as_symbol_id());
      fn_compiler.add_local(name);
      ++arity;
      p = Heap::cdr(p);
    }
    out_arity = arity;

    // Compile body forms
    Value b = body;
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
    chunk->code.push_back(OP_RETURN);
    out_upvals = fn_compiler.upvalues;
    return vm.heap.allocate<ObjClosure>(chunk, arity, false, static_cast<uint32_t>(out_upvals.size()), static_cast<uint32_t>(fn_compiler.max_locals));
  }

private:
  void compile_expr(Value form, BytecodeChunk &chunk, bool is_tail = false) {
    // 1. Self-evaluating literals
    if (form.is_int() || form.is_double() || form.is_bool() || form.is_nil() || form.is_unspecified() || form.is_keyword() || Heap::is_string(form) || Heap::is_vector(form) || Heap::is_map(form)) {
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
          emit_constant(Heap::car(rest), chunk);
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
            // (define (fn args...) body...) => (define fn (lambda (args...) body...))
            Value fn_name = Heap::car(target);
            Value fn_args = Heap::cdr(target);
            uint32_t arity = 0;
            std::vector<UpvalueDesc> upvals;
            ObjClosure *closure = compile_function(fn_args, def_body, arity, upvals);
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
          patch_jump(jump_false_ix + 1, static_cast<uint16_t>(else_target - (jump_false_ix + 3)), chunk);

          // Else branch
          if (Heap::is_cons(else_expr)) {
            compile_expr(Heap::car(else_expr), chunk, is_tail);
          } else {
            chunk.code.push_back(OP_UNSPECIFIED);
          }

          // Patch jump_exit
          size_t exit_target = chunk.code.size();
          patch_jump(jump_exit_ix + 1, static_cast<uint16_t>(exit_target - (jump_exit_ix + 3)), chunk);
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
          ObjClosure *closure = compile_function(Value::nil(), rest, arity, upvals);
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

        // (yield)
        if (op_name == "yield") {
          chunk.code.push_back(OP_YIELD);
          chunk.code.push_back(OP_UNSPECIFIED);
          return;
        }

        // (let ((v e)...) body...) or (let [v e ...] body...)
        if (op_name == "let") {
          Value bindings = Heap::car(rest);
          Value let_body = Heap::cdr(rest);

          // Check if named let: (let loop ((v e)...) body...) or (let loop [v e ...] body...)
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
            ObjClosure *closure = compile_function(param_list, let_body, arity, upvals, loop_str);

            // Push callee closure and initial arguments, then call
            emit_closure(closure, upvals, chunk);
            for (Value init_expr : inits) {
              compile_expr(init_expr, chunk, false);
            }
            chunk.code.push_back(is_tail ? OP_TAIL_CALL : OP_CALL);
            chunk.code.push_back(static_cast<uint8_t>(inits.size()));
            return;
          }

          // Standard let: compile initializers into local slots
          size_t initial_locals = locals.size();

          auto pairs = parse_bindings(bindings);
          for (const auto &p : pairs) {
            compile_expr(p.val, chunk, false);
            int slot = add_local(vm.get_symbol_name(p.var.as_symbol_id()));
            emit_op(OP_SET_LOCAL, static_cast<uint16_t>(slot), chunk);
            chunk.code.push_back(OP_POP);
          }

          Value cur_body = let_body;
          while (Heap::is_cons(cur_body)) {
            bool is_last = Heap::cdr(cur_body).is_nil();
            compile_expr(Heap::car(cur_body), chunk, is_last && is_tail);
            if (!is_last) chunk.code.push_back(OP_POP);
            cur_body = Heap::cdr(cur_body);
          }

          locals.resize(initial_locals);
          return;
        }

        // (when test body...)
        if (op_name == "when") {
          Value test_expr = Heap::car(rest);
          Value body_forms = Heap::cdr(rest);
          Value begin_sym = Value::from_symbol_id(vm.intern("begin"));
          Value if_sym = Value::from_symbol_id(vm.intern("if"));
          Value desugared = vm.heap.cons(if_sym,
            vm.heap.cons(test_expr,
              vm.heap.cons(vm.heap.cons(begin_sym, body_forms),
                vm.heap.cons(Value::unspecified(), Value::nil()))));
          compile_expr(desugared, chunk, is_tail);
          return;
        }

        // (unless test body...)
        if (op_name == "unless") {
          Value test_expr = Heap::car(rest);
          Value body_forms = Heap::cdr(rest);
          Value begin_sym = Value::from_symbol_id(vm.intern("begin"));
          Value if_sym = Value::from_symbol_id(vm.intern("if"));
          Value desugared = vm.heap.cons(if_sym,
            vm.heap.cons(test_expr,
              vm.heap.cons(Value::unspecified(),
                vm.heap.cons(vm.heap.cons(begin_sym, body_forms), Value::nil()))));
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

          Value if_sym = Value::from_symbol_id(vm.intern("if"));
          Value begin_sym = Value::from_symbol_id(vm.intern("begin"));
          Value else_sym = Value::from_symbol_id(vm.intern("else"));

          Value current = Value::unspecified();

          for (auto it = clause_vec.rbegin(); it != clause_vec.rend(); ++it) {
            Value clause = *it;
            Value test = Heap::car(clause);
            Value body = Heap::cdr(clause);
            Value body_seq = vm.heap.cons(begin_sym, body);

            if (test.is_symbol() && test.as_symbol_id() == else_sym.as_symbol_id()) {
              current = body_seq;
            } else {
              current = vm.heap.cons(if_sym,
                          vm.heap.cons(test,
                            vm.heap.cons(body_seq,
                              vm.heap.cons(current, Value::nil()))));
            }
          }

          compile_expr(current, chunk, is_tail);
          return;
        }

        // (let* ((v1 e1) (v2 e2)...) body...) or (let* [v1 e1 ...] body...)
        if (op_name == "let*") {
          Value bindings = Heap::car(rest);
          Value body = Heap::cdr(rest);
          Value let_sym = Value::from_symbol_id(vm.intern("let"));

          auto pairs = parse_bindings(bindings);
          if (pairs.empty()) {
            Value begin_sym = Value::from_symbol_id(vm.intern("begin"));
            compile_expr(vm.heap.cons(begin_sym, body), chunk, is_tail);
            return;
          }

          Value current = vm.heap.cons(let_sym,
                            vm.heap.cons(vm.heap.cons(
                              vm.heap.cons(pairs.back().var, vm.heap.cons(pairs.back().val, Value::nil())),
                              Value::nil()), body));

          for (int i = static_cast<int>(pairs.size()) - 2; i >= 0; --i) {
            current = vm.heap.cons(let_sym,
                        vm.heap.cons(vm.heap.cons(
                          vm.heap.cons(pairs[i].var, vm.heap.cons(pairs[i].val, Value::nil())),
                          Value::nil()),
                          vm.heap.cons(current, Value::nil())));
          }

          compile_expr(current, chunk, is_tail);
          return;
        }

        // (do ((var init [step]) ...) (test expr...) cmd...) or (do [var init [step] ...] ...)
        if (op_name == "do") {
          Value var_clauses = Heap::car(rest);
          Value test_clause = Heap::car(Heap::cdr(rest));
          Value commands = Heap::cdr(Heap::cdr(rest));

          Value test_expr = Heap::car(test_clause);
          Value result_exprs = Heap::cdr(test_clause);

          std::vector<Value> bindings;
          std::vector<Value> step_args;

          if (Heap::is_cons(var_clauses) && Heap::car(var_clauses).is_symbol() &&
              vm.get_symbol_name(Heap::car(var_clauses).as_symbol_id()) == "vector") {
            Value elems = Heap::cdr(var_clauses);
            while (Heap::is_cons(elems)) {
              Value v_name = Heap::car(elems);
              elems = Heap::cdr(elems);
              Value v_init = Heap::is_cons(elems) ? Heap::car(elems) : Value::unspecified();
              if (Heap::is_cons(elems)) elems = Heap::cdr(elems);
              Value v_step = v_name;
              if (Heap::is_cons(elems) && !Heap::car(elems).is_symbol()) {
                v_step = Heap::car(elems);
                elems = Heap::cdr(elems);
              }
              bindings.push_back(vm.heap.cons(v_name, vm.heap.cons(v_init, Value::nil())));
              step_args.push_back(v_step);
            }
          } else {
            Value vc = var_clauses;
            while (Heap::is_cons(vc)) {
              Value item = Heap::car(vc);
              Value v_name = Heap::car(item);
              Value v_init = Heap::car(Heap::cdr(item));
              Value v_step = Heap::is_cons(Heap::cdr(Heap::cdr(item))) ?
                             Heap::car(Heap::cdr(Heap::cdr(item))) : v_name;

              bindings.push_back(vm.heap.cons(v_name, vm.heap.cons(v_init, Value::nil())));
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

          Value loop_sym = Value::from_symbol_id(vm.intern("loop"));
          Value let_sym = Value::from_symbol_id(vm.intern("let"));
          Value if_sym = Value::from_symbol_id(vm.intern("if"));
          Value begin_sym = Value::from_symbol_id(vm.intern("begin"));

          Value loop_call = vm.heap.cons(loop_sym, step_list);

          Value else_branch = vm.heap.cons(begin_sym, vm.heap.cons(loop_call, Value::nil()));
          if (Heap::is_cons(commands)) {
            std::vector<Value> cmd_vec;
            Value c = commands;
            while (Heap::is_cons(c)) {
              cmd_vec.push_back(Heap::car(c));
              c = Heap::cdr(c);
            }
            cmd_vec.push_back(loop_call);
            Value cmd_list = Value::nil();
            for (auto it = cmd_vec.rbegin(); it != cmd_vec.rend(); ++it) {
              cmd_list = vm.heap.cons(*it, cmd_list);
            }
            else_branch = vm.heap.cons(begin_sym, cmd_list);
          }

          Value then_branch = vm.heap.cons(begin_sym, result_exprs);

          Value if_form = vm.heap.cons(if_sym,
                            vm.heap.cons(test_expr,
                              vm.heap.cons(then_branch,
                                vm.heap.cons(else_branch, Value::nil()))));

          Value desugared = vm.heap.cons(let_sym,
                              vm.heap.cons(loop_sym,
                                vm.heap.cons(binding_list,
                                  vm.heap.cons(if_form, Value::nil()))));

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

          Value if_sym = Value::from_symbol_id(vm.intern("if"));
          Value current = exprs.back();

          for (int i = static_cast<int>(exprs.size()) - 2; i >= 0; --i) {
            current = vm.heap.cons(if_sym,
                        vm.heap.cons(exprs[i],
                          vm.heap.cons(current,
                            vm.heap.cons(Value::boolean_false(), Value::nil()))));
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

          Value if_sym = Value::from_symbol_id(vm.intern("if"));
          Value current = exprs.back();

          for (int i = static_cast<int>(exprs.size()) - 2; i >= 0; --i) {
            current = vm.heap.cons(if_sym,
                        vm.heap.cons(exprs[i],
                          vm.heap.cons(Value::boolean_true(),
                            vm.heap.cons(current, Value::nil()))));
          }

          compile_expr(current, chunk, is_tail);
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

  void emit_closure(ObjClosure *proto, const std::vector<UpvalueDesc> &upvals, BytecodeChunk &chunk) {
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

  void patch_jump(size_t offset_index, uint16_t jump_amount, BytecodeChunk &chunk) {
    chunk.code[offset_index] = static_cast<uint8_t>((jump_amount >> 8) & 0xFF);
    chunk.code[offset_index + 1] = static_cast<uint8_t>(jump_amount & 0xFF);
  }

  int add_local(const std::string &name) {
    locals.push_back({name, scope_depth, false});
    if (locals.size() > max_locals) max_locals = locals.size();
    return static_cast<int>(locals.size() - 1);
  }

  int resolve_local(const std::string &name) {
    for (int i = static_cast<int>(locals.size()) - 1; i >= 0; --i) {
      if (locals[i].name == name) return i;
    }
    return -1;
  }

  int resolve_upvalue(const std::string &name) {
    if (!parent) return -1;

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

  struct BindingPair {
    Value var;
    Value val;
  };

  std::vector<BindingPair> parse_bindings(Value bindings) {
    std::vector<BindingPair> result;
    if (bindings.is_nil()) return result;

    // Check if bracketed vector: (vector var1 val1 var2 val2 ...)
    if (Heap::is_cons(bindings) && Heap::car(bindings).is_symbol() &&
        vm.get_symbol_name(Heap::car(bindings).as_symbol_id()) == "vector") {
      Value elems = Heap::cdr(bindings);
      while (Heap::is_cons(elems)) {
        Value var = Heap::car(elems);
        elems = Heap::cdr(elems);
        Value val = Heap::is_cons(elems) ? Heap::car(elems) : Value::unspecified();
        if (Heap::is_cons(elems)) elems = Heap::cdr(elems);
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
        Value val = Heap::is_cons(Heap::cdr(pair)) ? Heap::car(Heap::cdr(pair)) : Value::unspecified();
        result.push_back({var, val});
      }
      b = Heap::cdr(b);
    }
    return result;
  }

  VM &vm;
  Compiler *parent;
  std::vector<Local> locals;
  std::vector<UpvalueDesc> upvalues;
  int scope_depth;
  size_t max_locals;
};

} // namespace vxs
