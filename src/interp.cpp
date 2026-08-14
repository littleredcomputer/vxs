//----------------------------------------------------------------------
// vx-scheme : Scheme interpreter.
// Copyright (c) 2002,2003,2006 and onwards Colin Smith.
//
// You may distribute under the terms of the Artistic License,
// as specified in the LICENSE file.
//
// interp.cpp : SICP-style R4Rs-compliant Scheme interpreter

#include "vx-scheme.h"

// --------------------------------------------------------------------------
// Populate the symbol table with builtins.
//

psymbol s_and = intern("and");
psymbol s_apply = intern("apply");
psymbol s_begin = intern("begin");
psymbol s_callcc = intern("call-with-current-continuation");
psymbol s_callwif = intern("call-with-input-file");
psymbol s_callwof = intern("call-with-output-file");
psymbol s_case = intern("case");
psymbol s_cond = intern("cond");
psymbol s_define = intern("define");
psymbol s_defmacro = intern("defmacro");
psymbol s_delay = intern("delay");
psymbol s_do = intern("do");
psymbol s_else = intern("else");
psymbol s_eval = intern("eval");
psymbol s_force = intern("force");
psymbol s_foreach = intern("for-each");
psymbol s_if = intern("if");
psymbol s_lambda = intern("lambda");
psymbol s_let = intern("let");
psymbol s_letrec = intern("letrec");
psymbol s_letstar = intern("let*");
psymbol s_load = intern("load");
psymbol s_map = intern("map");
psymbol s_or = intern("or");
psymbol s_passto = intern("=>");
psymbol s_set = intern("set!");
psymbol s_time = intern("time");
psymbol s_withinput = intern("with-input-from-file");
psymbol s_withoutput = intern("with-output-to-file");

// --------------------------------------------------------------------------
// Unsafe Accessors
//
// These are versions of car and cdr that do not check to ensure that
// they are applied to conses.  The program can crash if this precondition
// is not meant.  They are only safe to use when the implementation can
// guarantee that they are applied to conses (and so we only use them
// on lists that we manage explicitly).

// Macros CAR and CDR have been removed in favor of unsafe_car() and unsafe_cdr()

void Context::bind(Cell *env, Cell *c, Cell *value) {
  cellvector *vec = car(env)->VectorValue();
  psymbol s = c->SymbolValue();

  if (c->is_quickened() && c->e_skip() == 0) {
    // We have a quick binding, and, as we expect,
    // it's within this frame.  We can establish
    // it without searching.

    int b_skip = c->b_skip();
    int sz = vec->size();

    if (b_skip == sz) {
      r_nu = cons(c, value);
      vec->push(r_nu);
    } else if (b_skip < 0 || b_skip > sz)
      error("internal error: invalid lexical address: ", c->SymbolValue()->key);
    else
      Cell::setcdr(vec->get(b_skip), value);
  } else {
    for (int ix = 0; ix < vec->size(); ++ix) {
      Cell *c = vec->get(ix);
      if (car(c)->SymbolValue() == s) {
        Cell::setcdr(c, value);
        return;
      }
    }
    // construct new binding element (being carful that
    // intermediate material is reachable from the register
    // set.
    r_nu = make_symbol(s);
    r_nu = cons(r_nu, value);
    vec->push(r_nu);
  }
}

// This is a list of "states" the evaluator can be in.  The names
// were chosen to harmonize with those chosen in SICP.

enum {
  eval_dispatch,
  eval_complete,
  ev_application,
  ev_application2,
  ev_args1,
  ev_args2,
  ev_sequence,
  ev_sequence_continue,
  apply_dispatch,
  apply_dispatch2,
  ev_if,
  ev_if_decide,
  ev_finish,
  ev_define,
  ev_define_1,
  ev_eval,
  ev_eval1,
  ev_set,
  ev_set_1,
  ev_or,
  ev_or2,
  ev_and,
  ev_and2,
  macro_subst,
  ev_let,
  ev_let_init,
  let_accumulate_binding,
  ev_letstar,
  ev_letstar_init,
  ev_letstar_bind,
  ev_do,
  ev_do_init,
  ev_do_bind,
  ev_do_test,
  ev_after_test,
  ev_do_step,
  ev_step_1,
  ev_step_bind,
  ev_step_finish,
  ev_do_step_2,
  ev_cond,
  ev_cond_test,
  ev_apply,
  ev_apply2,
  ev_apply3,
  macro_subst2,
  ev_cond_passto,
  ev_quasiquote,
  ev_unquote,
  ev_qq0,
  ev_qq1,
  ev_qq2,
  ev_qq3,
  ev_qq_decrease,
  ev_qqd_1,
  ev_qq_finish,
  ev_apply4,
  ev_unq_spl,
  ev_unq_spl2,
  ev_letrec,
  ev_letrec1,
  ev_letrec2,
  ev_case,
  ev_case2,
  ev_foreach,
  ev_foreach1,
  ev_foreach2,
  ev_foreach3,
  ev_foreach4,
  ev_force,
  ev_force2,
  ev_map,
  ev_map1,
  ev_map2,
  ev_map3,
  ev_withinput,
  ev_withinput1,
  ev_withinput2,
  ev_withoutput,
  ev_withoutput1,
  ev_withoutput2,
  ev_load,
  ev_load2,
  ev_callwof,
  ev_callwof2,
  ev_time,
  ev_time1,
};

// These are the above states in string form.  This is only used
// for debugging, to dump the evaluator's state transitions.

static const char *state_name[] = {
    "eval_dispatch",
    "eval_complete",
    "ev_application",
    "ev_application2",
    "ev_args1",
    "ev_args2",
    "ev_sequence",
    "ev_sequence_continue",
    "apply_dispatch",
    "apply_dispatch2",
    "ev_eval",
    "ev_eval1",
    "ev_if",
    "ev_if_decide",
    "ev_finish",
    "ev_define",
    "ev_define_1",
    "ev_set",
    "ev_set_1",
    "ev_or",
    "ev_or2",
    "ev_and",
    "ev_and2",
    "macro_subst",
    "ev_let",
    "ev_let_init",
    "let_accumulate_binding",
    "ev_letstar",
    "ev_letstar_init",
    "ev_letstar_bind",
    "ev_do",
    "ev_do_init",
    "ev_do_bind",
    "ev_do_test",
    "ev_after_test",
    "ev_do_step",
    "ev_step_1",
    "ev_step_bind",
    "ev_step_finish",
    "ev_do_step_2",
    "ev_cond",
    "ev_cond_test",
    "ev_apply",
    "ev_apply2",
    "ev_apply3",
    "macro_subst2",
    "ev_cond_passto",
    "ev_quasiquote",
    "ev_unquote",
    "ev_qq0",
    "ev_qq1",
    "ev_qq2",
    "ev_qq3",
    "ev_qq_decrease",
    "ev_qqd_1",
    "ev_qq_finish",
    "ev_apply4",
    "ev_unq_spl",
    "ev_unq_spl2",
    "ev_letrec",
    "ev_letrec1",
    "ev_letrec2",
    "ev_case",
    "ev_case2",
    "ev_foreach",
    "ev_foreach1",
    "ev_foreach2",
    "ev_foreach3",
    "ev_foreach4",
    "ev_force",
    "ev_force2",
    "ev_map",
    "ev_map1",
    "ev_map2",
    "ev_map3",
    "ev_withinput",
    "ev_withinput2",
    "ev_withoutput",
    "ev_withoutput2",
    "ev_load",
    "ev_load2",
    "ev_callwof",
    "ev_callwof2",
    "ev_time",
    "ev_time1",
};

// Here it is: a >1000 line function that consists of a giant switch
// statement, and it's peppered with goto's!
//
// This would be inexcusable, except it has to do its job (evaulation
// Scheme expressions) without using the C stack as a resource.  This
// could be thought of as a sort of microcode, using a set of virtual
// machine registers (r_exp, r_unev, etc.: the uses of these registers
// is described in SICP).
//
// I've followed this approach because I wanted to be able to capture
// continuations and collect garbage without having to know any details
// about how the C stack operates.  This evaluator is somewhat more
// complicated than the one described in SICP since it supports many
// language features not discussed in that chapter.
//
// For these reasons, eval should not be recursed into by anyone,
// including itself.

Cell *Context::interp_evaluator(Cell *form) {
  psymbol s;
  Cell::Type t;
  Cell::Procedure lambda;
  intptr_t flag = 0;
  double t1;
  bool trace;
  psymbol p;
  sstring read_sstr;
  init_machine();
  state = eval_dispatch;
  r_cont = eval_complete;
  r_exp = form;
  r_qq = 0;
  trace = debug_flag(TRACE_EVAL);

#define GOTO(x)                                                                \
  do {                                                                         \
    state = x;                                                                 \
    goto TOP;                                                                  \
  } while (0)

#define EVAL_DISPATCH()                                                        \
  do {                                                                         \
    if (r_exp == nil) {                                                        \
      r_val = nil;                                                             \
      GOTO(r_cont);                                                            \
    }                                                                          \
    Cell::Type __t = (r_exp)->type();                                          \
    if (__t == Cell::Type::Cons)                                               \
      GOTO(ev_application);                                                    \
    if (__t == Cell::Type::Symbol)                                             \
      r_val = get(r_env, r_exp);                                               \
    else                                                                       \
      r_val = r_exp;                                                           \
    GOTO(r_cont);                                                              \
  } while (0)

#define RETURN_VALUE(v)                                                        \
  do {                                                                         \
    r_val = (v);                                                               \
    restore_i(r_cont);                                                         \
    GOTO(r_cont);                                                              \
  } while (0)

// If the exp is self-evaluating or a variable, handle it
// immediately.  Else call eval_dispatch in a context
// where r_env end r_unev are saved/restored.

#define CALL_EVAL(label)                                                       \
  t = (r_exp)->type();                                                         \
  if (t == Cell::Type::Symbol) {                                               \
    r_val = get(r_env, r_exp);                                                 \
    goto label##__2;                                                           \
  } else if (t != Cell::Type::Cons) {                                          \
    r_val = r_exp;                                                             \
    goto label##__2;                                                           \
  } else {                                                                     \
    save(r_env);                                                               \
    save(r_unev);                                                              \
    r_cont = label;                                                            \
    GOTO(eval_dispatch);                                                       \
  }                                                                            \
  case label:                                                                  \
    restore(r_unev);                                                           \
    restore(r_env);                                                            \
    label##__2:

TOP:

  if (trace)
    print_vm_state();

  switch (state) {
  case eval_dispatch:

    if (r_exp == nil) {
      r_val = nil;
      GOTO(r_cont);
    }

    switch (r_exp->type()) {
    case Cell::Type::Symbol:
      r_val = get(r_env, r_exp);
      GOTO(r_cont);
    case Cell::Type::Cons:
      GOTO(ev_application);
    default: // self-evaluating
      r_val = r_exp;
      GOTO(r_cont);
    }

  case eval_complete:
    return r_val;

  case ev_application:
    save_i(r_cont);
    r_unev = cdr(r_exp);
    r_exp = car(r_exp);
    CALL_EVAL(ev_application2);
    r_proc = r_val;

  case apply_dispatch:

    if (!r_proc->flag(Cell::Flag::Macro)) {
      // It's not a special form: evaluate all the arguments
      // in unev and collect them into r_argl.

      clear(r_argl);
      save(r_proc);

    case ev_args1:
      if (r_unev == nil) {
        restore(r_proc);
        GOTO(apply_dispatch2);
      }

      save(r_argl);
      r_exp = car(r_unev);
      r_unev = cdr(r_unev);
      CALL_EVAL(ev_args2);
      restore(r_argl);
      l_append(r_argl, r_val);
      GOTO(ev_args1);
    }

  case apply_dispatch2:

    switch (r_proc->type()) {
    case Cell::Type::Builtin:
      // =================================================
      //            THE BUILTIN SPECIAL FORMS
      // =================================================

      s = r_proc->BuiltinValue();

      if (s == s_if)
        GOTO(ev_if);
      else if (s == s_define)
        GOTO(ev_define);
      else if (s == s_begin)
        GOTO(ev_sequence);
      else if (s == s_set)
        GOTO(ev_set);
      else if (s == s_let)
        GOTO(ev_let);
      else if (s == s_letstar)
        GOTO(ev_letstar);
      else if (s == s_letrec)
        GOTO(ev_letrec);
      else if (s == s_do)
        GOTO(ev_do);
      else if (s == s_cond)
        GOTO(ev_cond);
      else if (s == s_case)
        GOTO(ev_case);
      else if (s == s_eval)
        GOTO(ev_eval);
      else if (s == s_foreach)
        GOTO(ev_foreach);
      else if (s == s_load)
        GOTO(ev_load);
      else if (s == s_map)
        GOTO(ev_map);
      else if (s == s_apply)
        GOTO(ev_apply);
      else if (s == s_force)
        GOTO(ev_force);
      else if (s == s_quote)
        r_val = car(r_unev);
      else if (s == s_or) {
        r_val = &Cell::Bool_F;
        GOTO(ev_or);
      } else if (s == s_and) {
        r_val = &Cell::Bool_T;
        GOTO(ev_and);
      } else if (s == s_delay)
        r_val = make_promise(r_env, r_unev);
      else if (s == s_quasiquote) {
        r_unev = car(r_unev);
        GOTO(ev_quasiquote);
      } else if (s == s_lambda)
        r_val = make_procedure(r_env, cdr(r_unev), car(r_unev));
      else if (s == s_defmacro) {
        r_proc = make_macro(r_env, cdr(r_unev), cdar(r_unev));
        bind(r_env, caar(r_unev), r_proc);
        r_val = unspecified;
      } else if (s == s_time)
        GOTO(ev_time);
      else if (s == s_withinput)
        GOTO(ev_withinput);
      else if (s == s_withoutput)
        GOTO(ev_withoutput);
      else if (s == s_callwof)
        GOTO(ev_callwof);
      else if (s == s_callwif) {
        r_proc = Cell::cadar(&r_argl);
        r_tmp = make_iport(Cell::caar(&r_argl)->StringValue());
        r_tmp = cons(r_tmp, nil);
        Cell::setcar(&r_argl, r_tmp);
        GOTO(apply_dispatch2);
      } else if (s == s_callcc) {
        r_proc = Cell::caar(&r_argl);
        r_tmp = make_continuation();
        r_tmp = cons(r_tmp, nil);
        Cell::setcar(&r_argl, r_tmp);
        GOTO(apply_dispatch2);
      } else
        error("unimplemented builtin ", s->key);
      break;

    case Cell::Type::Subr:
      r_val = r_proc->SubrValue()->subr(this, Cell::car(&r_argl));
      break;

    case Cell::Type::Lambda:
      lambda = r_proc->LambdaValue();

      if (r_proc->flag(Cell::Flag::Macro)) {
        save(r_env);

        r_env = extend(lambda.envt);
        bind_arguments(r_env, lambda.arglist, r_unev);
        save_i(macro_subst); // continuation
      } else {
        r_env = extend(lambda.envt);
        bind_arguments(r_env, lambda.arglist, Cell::car(&r_argl));
      }

      r_unev = lambda.body;
      GOTO(ev_sequence);

    case Cell::Type::Cont:
      r_val = Cell::caar(&r_argl);
      load_continuation(r_proc);
      break;

    case Cell::Type::Cproc:
      if (vm_execute) {
        (this->*vm_execute)(r_proc, Cell::car(&r_argl));
      } else {
        error("VM not loaded: can't dispatch a compiled procedure");
      }
      break;

    default:
      r_proc->dump(stdout);
      error("can't dispatch one of those.");
    }

    restore_i(r_cont);
    GOTO(r_cont);

  case ev_eval:
    // The eval special form.  (Can't let eval recurse, so
    // we take care of it here in the VM).

    r_exp = Cell::caar(&r_argl);
    save(r_env);
    r_env = root_envt;
    CALL_EVAL(ev_eval1);
    restore(r_env);
    RETURN_VALUE(r_val);

  case ev_time:
    // Call the supplied procedure while timing it.
    // Return a cons of the elapsed time and the proc's
    // value.

    r_proc = Cell::caar(&r_argl);

    clear(r_argl);
    save(make_real(vx_get_time()));
    save_i(ev_time1); // cont
    GOTO(apply_dispatch2);

  case ev_time1:

    t1 = vx_get_time();
    restore(r_tmp);
    r_tmp = make_real(t1 - r_tmp->RealValue());
    RETURN_VALUE(cons(r_tmp, r_val));

  case ev_sequence:
    r_exp = car(r_unev);

    if (r_exp == nil)
      RETURN_VALUE(unspecified);

    if (cdr(r_unev) == nil) {
      restore_i(r_cont);
      EVAL_DISPATCH();
    }

    CALL_EVAL(ev_sequence_continue);

    r_unev = cdr(r_unev);
    GOTO(ev_sequence);

  // ev_if, etc., is a deviation from the presentation in SICP.
  // In brief we are not dispatched by syntax analysis, but by
  // finding a Builtin in the functor position.  We get here via
  // apply dispatch.  Our job is to compute `r_val', pop the
  // continuation and branch there.
  //
  // Had we followed SICP strictly, then special forms like
  // "if" would have no visible definition at all, and could
  // never be redefined.  Most Scheme interpretations do allow
  // for the redefinition of builtin symbols.  Hence we use
  // builtins as a sort of "flag" that invokes the internal
  // sytnax-directed implementation to proceed.

  case ev_if:
    r_exp = car(r_unev);
    r_unev = cdr(r_unev);
    CALL_EVAL(ev_if_decide);

    restore_i(r_cont);

    if (r_val->istrue()) {
      r_exp = car(r_unev);
      EVAL_DISPATCH();
    } else {
      if ((r_exp = cdr(r_unev)) != nil) {
        r_exp = car(r_exp);
        EVAL_DISPATCH();
      } else {
        r_val = unspecified;
        GOTO(r_cont);
      }
    }

  case ev_define:
    r_tmp = car(r_unev);

    if (r_tmp->type() == Cell::Type::Symbol) {
      save(r_env);
      save(r_unev);
      r_exp = cadr(r_unev);
      r_cont = ev_define_1;
      EVAL_DISPATCH();
    } else {
      r_proc = make_procedure(r_env, cdr(r_unev), cdr(r_tmp));
      bind(r_env, car(r_tmp), r_proc);
      RETURN_VALUE(unspecified);
    }

  case ev_define_1:
    restore(r_unev);
    restore(r_env);
    bind(r_env, car(r_unev), r_val);
    RETURN_VALUE(unspecified);

  case ev_set:
    r_exp = cadr(r_unev);
    CALL_EVAL(ev_set_1);

    set(r_env, car(r_unev), r_val);
    RETURN_VALUE(unspecified);

  case ev_or:
    if (r_unev == nil || r_val->istrue()) {
      restore_i(r_cont);
      GOTO(r_cont);
    }

    r_exp = car(r_unev);
    CALL_EVAL(ev_or2);
    r_unev = cdr(r_unev);
    GOTO(ev_or);

  case ev_and:
    if (r_unev == nil || !r_val->istrue()) {
      restore_i(r_cont);
      GOTO(r_cont);
    }

    r_exp = car(r_unev);
    CALL_EVAL(ev_and2);
    r_unev = cdr(r_unev);
    GOTO(ev_and);

  case macro_subst:

    // The macro has been expanded.  One more trip through
    // eval, please.

    restore(r_env);
    restore_i(r_cont);
    r_exp = r_val;
    EVAL_DISPATCH();

  case ev_let:
    // (let [name?] ((v e) ...) x...)
    // The plan is to accumulate the list of variables (v) in
    // r_varl, and the list of initializers (e) in r_argl.

    if (car(r_unev)->type() == Cell::Type::Symbol) {
      r_proc = car(r_unev); // named let: stash in r_proc
      r_unev = cdr(r_unev);
    } else
      r_proc = nil;

    clear(r_argl);
    clear(r_varl);

    if (car(r_unev) == nil) {
      r_unev = cdr(r_unev); // (let () x...)
      r_env = extend(r_env);
      goto let_noargs;
    }

    save(r_proc);
    save(cdr(r_unev));
    r_unev = car(r_unev); // fall through

  case ev_let_init:
    save(r_argl);
    save(r_varl);
    r_exp = cadar(r_unev);

    CALL_EVAL(let_accumulate_binding);

    restore(r_varl);
    restore(r_argl);

    l_append(r_varl, caar(r_unev));
    l_append(r_argl, r_val);
    r_unev = cdr(r_unev);
    if (r_unev == nil) {
      restore(r_unev);
      restore(r_proc);
    let_noargs:
      if (r_proc != nil) {
        r_env = extend(r_env);
        r_tmp = make_procedure(r_env, r_unev, Cell::car(&r_varl));
        bind(r_env, r_proc, r_tmp);
      }
      r_env = extend(r_env);
      bind_arguments(r_env, Cell::car(&r_varl), Cell::car(&r_argl));
      GOTO(ev_sequence);
    }

    GOTO(ev_let_init);

  case ev_letstar:
    // (let* ((v e) ...) x1 ...)
    // We unpack, bind, and extend in a loop, until we're
    // ready for the sequence.
    if (car(r_unev) == nil) {
      r_env = extend(r_env);
      r_unev = cdr(r_unev);
      GOTO(ev_sequence);
    }

    save(cdr(r_unev));
    r_unev = car(r_unev);

  /* fall thru */

  case ev_letstar_init:
    save(r_env);
    save(r_unev);
    r_exp = cadar(r_unev);
    r_cont = ev_letstar_bind;
    EVAL_DISPATCH();

  case ev_letstar_bind:
    restore(r_unev);
    restore(r_env);
    r_env = extend(r_env);
    bind(r_env, caar(r_unev), r_val);
    r_unev = cdr(r_unev);
    if (r_unev == nil) {
      restore(r_unev);
      GOTO(ev_sequence);
    }

    GOTO(ev_letstar_init);

  case ev_letrec:
    // we have: (((v1 i1) (v2 i2)...) x1 x2...)
    clear(r_varl);
    clear(r_argl);
    save(cdr(r_unev));

    r_env = extend(r_env);
    for (r_exp = car(r_unev); r_exp != nil; r_exp = cdr(r_exp)) {
      l_append(r_varl, caar(r_exp));
      bind(r_env, caar(r_exp), &Cell::Error);
    }

    save(r_varl);
    r_unev = car(r_unev);

  case ev_letrec1:
    if (r_unev != nil) {
      r_exp = cadar(r_unev);
      save(r_argl);
      CALL_EVAL(ev_letrec2);
      restore(r_argl);
      l_append(r_argl, r_val);
      r_unev = cdr(r_unev);
      GOTO(ev_letrec1);
    }

    restore(r_varl);
    restore(r_unev);
    bind_arguments(r_env, Cell::car(&r_varl), Cell::car(&r_argl));
    GOTO(ev_sequence);

  case ev_do:
    // (do ((var init step)...) (test x...) y...)
    // Like let, accumulate variables (v) and
    // initializers (i) into r_varl and r_argl.

    save(r_unev);         // (((var init step)...) (test x...) y...)
    r_unev = car(r_unev); // ((var init step)...)
    clear(r_argl);
    clear(r_varl);
  /* fall through */

  case ev_do_init:
    save(r_argl);
    save(r_varl);
    save(r_env);
    save(r_unev);
    r_exp = cadar(r_unev);
    r_cont = ev_do_bind;
    EVAL_DISPATCH();

  case ev_do_bind:
    restore(r_unev);
    restore(r_env);
    restore(r_varl);
    restore(r_argl);

    l_append(r_varl, caar(r_unev));
    l_append(r_argl, r_val);
    r_unev = cdr(r_unev);

    if (r_unev == nil) {
      // All done with inits. Create environment and start testing.
      r_env = extend(r_env);
      bind_arguments(r_env, Cell::car(&r_varl), Cell::car(&r_argl));
      restore(r_unev); // (((var init step)...) (test x...) y...)
      GOTO(ev_do_test);
    }

    GOTO(ev_do_init);

  case ev_do_test:
    r_exp = caadr(r_unev);
    CALL_EVAL(ev_after_test);

    if (r_val->istrue()) {
      // test passed: end iteration.  Evaulate
      // `x' expressions as a sequence.

      r_unev = cdadr(r_unev);

      if (r_unev == nil) // no consequent expressions?
        RETURN_VALUE(unspecified);

      GOTO(ev_sequence);
    }

    // otherwise, evaluate the y expressions for effect, if there
    // are any

    if (cddr(r_unev) == nil)
      GOTO(ev_do_step_2);

    save(r_unev);
    save(r_env);
    r_unev = cddr(r_unev);
    save_i(ev_do_step);
    GOTO(ev_sequence);

  case ev_do_step:
    // then use the step expressions (if any) to rebind the
    // variables, and retest.

    restore(r_env);
    restore(r_unev); // (((var init step)...) (test x...) y...)

  case ev_do_step_2:
    save(r_unev);
    r_unev = car(r_unev); // ((var init step) ...)
    clear(r_argl);
    clear(r_varl);
  /* fall through */

  case ev_step_1:
    if (r_unev == nil) {
      // all done.

      GOTO(ev_step_finish);
    }

    r_tmp = cddar(r_unev);
    if (r_tmp == nil) {
      r_unev = cdr(r_unev);
      GOTO(ev_step_1);
    }

    save(r_argl);
    save(r_varl);
    r_exp = caddar(r_unev);
    CALL_EVAL(ev_step_bind);

    restore(r_varl);
    restore(r_argl);

    l_append(r_varl, caar(r_unev));
    l_append(r_argl, r_val);
    r_unev = cdr(r_unev);
    GOTO(ev_step_1);

  case ev_step_finish:
    bind_arguments(r_env, Cell::car(&r_varl), Cell::car(&r_argl));
    restore(r_unev);
    GOTO(ev_do_test);

  case ev_cond:
    if (r_unev == nil)
      RETURN_VALUE(unspecified);

    r_exp = caar(r_unev); // t1
    if (r_exp->is_symbol(s_else))
      r_val = &Cell::Bool_T;
    else {
      r_cont = ev_cond_test;
      CALL_EVAL(ev_cond_test);
    }

    if (r_val->istrue()) {
      r_unev = cdar(r_unev);
      r_tmp = car(r_unev);

      // Check for "=> r_proc" syntax
      if (r_tmp->is_symbol(s_passto)) {
        // We already have the argument.  Now, evaluate
        // r_proc, so we can apply it.

        save(r_val);
        r_unev = cdr(r_unev);
        r_exp = car(r_unev);

        CALL_EVAL(ev_cond_passto);

        r_proc = r_val;
        restore(r_val);
        Cell::setcar(&r_argl, cons(r_val, nil));
        GOTO(apply_dispatch2);
      }

      GOTO(ev_sequence);
    }

    r_unev = cdr(r_unev);
    GOTO(ev_cond);

  case ev_apply:
    // we have, e.g., (apply + 1 2 '(3 4))
    // and we want (+ 1 2 3 4).

    r_proc = Cell::caar(&r_argl); // peel off r_proc
    r_tmp = Cell::cdar(&r_argl);
    clear(r_argl);

    for (; r_tmp != nil; r_tmp = cdr(r_tmp))
      if (cdr(r_tmp) == nil) // fold the list
        l_appendtail(r_argl, car(r_tmp));
      else
        l_append(r_argl, car(r_tmp));

    GOTO(apply_dispatch2);

  case ev_quasiquote:
    // If it's a vector, convert it to a list and
    // save a flag.

    ++r_qq;
    t = r_unev->type();

    if (t == Cell::Type::Vec) {
      save_i(1);
      r_unev = vector_to_list(this, cons(r_unev, nil)); // yyy
    } else
      save_i(0);

    save_i(ev_qq_finish);
    r_val = nil;

  case ev_qq0:

    t = r_unev->type();
    if (t == Cell::Type::Cons) {
      r_exp = car(r_unev);

      if (r_exp->type() == Cell::Type::Symbol) {
        p = r_exp->SymbolValue();

        if (p == s_unquote) // unquote: evaluate sequel.
        {
          if (r_qq == 1) {
            r_exp = cadr(r_unev);
            --r_qq;
            CALL_EVAL(ev_unquote);
            ++r_qq;
          } else {
            r_tmp = make_symbol(s_unquote);
            save(r_tmp);
            GOTO(ev_qq_decrease);
          }
        } else if (p == s_quasiquote) // increase QQ level.
        {
          r_unev = cdr(r_unev);
          save_i(ev_qq1);
          GOTO(ev_quasiquote);
        case ev_qq1:
          r_tmp = make_symbol(s_quasiquote);
          r_val = cons(r_tmp, r_val);
        } else
          goto QQCONS;
      }

      else if (r_exp->type() == Cell::Type::Cons &&
               car(r_exp)->is_symbol(s_unquote_splicing)) {
        if (r_qq == 1) {
          // unquote_splicing: generate list, and splice it
          // in.  First evaluate to get the list.

          r_exp = cadr(r_exp);
          r_unev = cdr(r_unev);
          CALL_EVAL(ev_unq_spl);

          // r_val holds the list.  Install it into r_argl
          // (tracking head and tail).  Then evaluate
          // what follows.

          r_tmp = r_val;
          while (cdr(r_tmp) != nil)
            r_tmp = cdr(r_tmp);

          Cell::setcar(&r_argl, r_val);
          Cell::setcdr(&r_argl, r_tmp);

          save(r_argl);
          r_exp = r_unev;
          save_i(ev_unq_spl2);
          GOTO(ev_qq0);
        } else {
          r_tmp = make_symbol(s_unquote_splicing);
          save(r_tmp);
          GOTO(ev_qq_decrease);
        }

      case ev_unq_spl2:
        restore(r_argl);
        l_appendtail(r_argl, r_val);
        r_val = Cell::car(&r_argl);
      } else if (r_unev == nil)
        r_val = nil;
      else {
      QQCONS:              // "move quasiquotation inward"
        save(cdr(r_unev)); // cons (qq (car), qq (cdr))
        save_i(ev_qq2);    // new continuation
        r_unev = r_exp;
        GOTO(ev_qq0);
      case ev_qq2:
        restore(r_unev);
        save(r_val);
        save_i(ev_qq3);
        GOTO(ev_qq0);
      case ev_qq3:
        restore(r_exp);
        r_val = cons(r_exp, r_val);
      }
    } else
      r_val = r_unev; // atoms are self-evaluating

    restore_i(r_cont);
    GOTO(r_cont);

  case ev_qq_finish: // finished.  reconvert to
    restore_i(flag); // vector form if necessary.
    if (flag)
      r_val = vector_from_list(this, r_val);
    --r_qq;
    restore_i(r_cont);
    GOTO(r_cont);

  case ev_qq_decrease:
    // we get here because we saw unquote or unquote_splicing,
    // and we want to proceed with a decreased qq level instead
    // of evaluating it (because the qq level was too high
    // when we encountered the form).

    --r_qq;
    r_unev = cdr(r_unev);
    save_i(ev_qqd_1);
    GOTO(ev_qq0);
  case ev_qqd_1:
    restore(r_exp); // recover head symbol
    ++r_qq;
    RETURN_VALUE(cons(r_exp, r_val));

  case ev_case:
    // (key ((d1 d2...) x1 x2...) ((d3 d4...) x3 x4...))
    // evaluate key, and shift it away.

    r_exp = car(r_unev);
    r_unev = cdr(r_unev);
    CALL_EVAL(ev_case2);

    for (; r_unev != nil; r_unev = cdr(r_unev)) {
      r_exp = car(r_unev);
      r_tmp = car(r_exp);

      if (r_tmp->is_symbol(s_else)) {
        r_unev = cdr(r_exp);
        GOTO(ev_sequence);
      }
      for (; r_tmp != nil; r_tmp = cdr(r_tmp)) {
        if (r_val->eq(car(r_tmp))) {
          r_unev = cdr(r_exp);
          GOTO(ev_sequence);
        }
      }
    }

    RETURN_VALUE(unspecified);

  case ev_foreach:
    r_proc = Cell::caar(&r_argl); // (r_proc list...)
    r_unev = Cell::cdar(&r_argl);

  case ev_foreach1:
    if (car(r_unev) == nil)
      RETURN_VALUE(unspecified);

    clear(r_argl);
    for (r_tmp = r_unev; r_tmp != nil; r_tmp = cdr(r_tmp)) {
      l_append(r_argl, caar(r_tmp));
      Cell::setcar(r_tmp, cdar(r_tmp));
    }

    save(r_unev);
    save(r_proc);
    save_i(ev_foreach2);
    GOTO(apply_dispatch2);
  case ev_foreach2:
    restore(r_proc);
    restore(r_unev);
    GOTO(ev_foreach1);

  case ev_map:
    r_proc = Cell::caar(&r_argl);
    // copy r_argl to r_unev (less the first elt., which was the r_proc)
    r_unev = Cell::cdar(&r_argl);
    clear(r_varl);

  case ev_map1:
    if (car(r_unev) == nil) // no more arguments
      GOTO(ev_map3);

    clear(r_argl);
    for (r_tmp = r_unev; r_tmp != nil; r_tmp = cdr(r_tmp)) {
      l_append(r_argl, caar(r_tmp));
      Cell::setcar(r_tmp, cdar(r_tmp));
    }

    save(r_varl);
    save(r_unev);
    save(r_proc);
    save_i(ev_map2);
    GOTO(apply_dispatch2);
  case ev_map2:
    restore(r_proc);
    restore(r_unev);
    restore(r_varl);
    l_append(r_varl, r_val);
    GOTO(ev_map1);

  case ev_map3:
    RETURN_VALUE(Cell::car(&r_varl));

  case ev_force:
    r_exp = Cell::caar(&r_argl);
    if (r_exp->flag(Cell::Flag::Forced))
      r_val = r_exp->unsafe_vector_value()->get(0); // return memoized value
    else {
      // If we haven't forced the promise yet, then the cdr
      // is pointing to a unit vector containing the
      // procedure we must evaluate to get the value, which
      // we then memoize.

      clear(r_argl);
      r_proc = r_exp->unsafe_vector_value()->get(0);
      save(r_exp);
      save_i(ev_force2);
      GOTO(apply_dispatch2);
    case ev_force2:
      // Now, it can happen that the procedure we're
      // invoking can force its own value.  If we find that
      // the FORCED flag has magically become set as a
      // result of forcing, then we must accept that earlier
      // computation (we are "higher" on the evaluation
      // stack)...

      restore(r_exp);
      if (r_exp->flag(Cell::Flag::Forced))
        r_val = r_exp->unsafe_vector_value()->get(0);
      else {
        r_exp->unsafe_vector_value()->set(0, r_val);
        r_exp->flag(Cell::Flag::Forced, true);
      }
    }

    restore_i(r_cont);
    GOTO(r_cont);

  case ev_withinput:
    // (filename proc)

    with_input(Cell::caar(&r_argl)->StringValue());
    r_proc = Cell::cadar(&r_argl);
    clear(r_argl);
    save_i(ev_withinput2); // continuation
    GOTO(apply_dispatch2);

  case ev_withinput2:
    without_input();
    restore_i(r_cont);
    GOTO(r_cont);

  case ev_withoutput:
    with_output(Cell::caar(&r_argl)->StringValue());
    r_proc = Cell::cadar(&r_argl);
    clear(r_argl);
    save_i(ev_withoutput2); // continuation
    GOTO(apply_dispatch2);

  case ev_withoutput2:
    without_output();
    restore_i(r_cont);
    GOTO(r_cont);

  case ev_load:
    r_unev = make_iport(Cell::caar(&r_argl)->StringValue());
    save(r_unev); // let r_unev hold input stream
    save(r_env);

  case ev_load2:
    restore(r_env);
    restore(r_unev);
    r_exp = read(r_unev->IportValue());
    if (r_exp) {
      save(r_unev);
      save(r_env);
      r_env = root_envt; // read files into global scope
      r_cont = ev_load2; // loop
      EVAL_DISPATCH();
    }
    r_exp = nil;
    RETURN_VALUE(r_val);

  case ev_callwof:
    r_proc = Cell::cadar(&r_argl);
    r_unev = make_oport(Cell::caar(&r_argl)->StringValue());
    Cell::setcar(&r_argl, cons(r_unev, nil));
    save(r_unev);
    save_i(ev_callwof2); // cont
    GOTO(apply_dispatch2);

  case ev_callwof2:
    restore(r_unev);
    fflush(r_unev->OportValue());
    RETURN_VALUE(r_val);

  default:
    printf("IC = %x\n", state);
    error("internal: invalid continuation");
  }

  return unimplemented;
}

void Context::print_vm_state() {
  printf("%d %s exp=", m_stack.size(), state_name[state]);
  r_exp->write(stdout);
  printf(" unev=");
  r_unev->write(stdout);
  printf(" proc=");
  r_proc->write(stdout);
  printf(" val=");
  r_val->write(stdout);
  printf(" argl=");
  Cell::car(&r_argl)->write(stdout);
  printf(" varl=");
  Cell::car(&r_varl)->write(stdout);
  printf(" env=");
  if (r_env == root_envt)
    printf("#<root>");
  else
    Cell::car(r_env)->write(stdout);

  printf(" cont=%s q%d\n", state_name[r_cont], r_qq);
}

void Context::bind_arguments(Cell *env, Cell *variables, Cell *values) {
  Cell *var;
  Cell *val;

  if (variables->type() == Cell::Type::Cons) {
    for (var = variables, val = values; var != nil;
         var = cdr(var), val = cdr(val)) {
      bind(env, car(var), car(val));

      if (cdr(var)->type() == Cell::Type::Symbol) {
        // Implement "dotted tail" procedure call.  If
        // the cdr of var is another symbol, then this
        // was to the right of the "dot"; put all the rest
        // of the arguments in there. [SICP 2ed. p. 104, 183n;
        // R5RS 4.1.4]

        bind(env, cdr(var), cdr(val));
        break;
      }
    }
  } else {
    // "varargs" version: (lambda args body...)

    bind(env, variables, values);
  }
}

Cell *Context::get(Cell *env, Cell *c) {
  c->typecheck(Cell::Type::Symbol);
  Cell *pResult = find(env, c);

  if (!pResult || !pResult->unsafe_cdr())
    error("unbound variable ", c->SymbolValue()->key);

  Cell *res = pResult->unsafe_cdr();

  if (res->type() == Cell::Type::Magic)
    return res->unsafe_magic_box()->get_f(this, res->unsafe_magic_vp());
  else
    return res;
}

void Context::set(Cell *env, Cell *var, Cell *value) {
  Cell *target = find(env, var);
  Cell *d;
  psymbol s = var->SymbolValue();

  if (!target)
    error("unbound variable ", s->key);

  if ((d = cdr(target)) && d->type() == Cell::Type::Magic)
    d->unsafe_magic_box()->set_f(this, d->unsafe_magic_vp(), value);
  else
    Cell::setcdr(target, value);
}

Cell *Context::find(Cell *env, Cell *c) {
  int e_count = 0;
  int b_count = 0;
  psymbol s = c->SymbolValue();
  Cell *e = env;
  Cell *val;

  if (c->is_quickened()) {
    int e_skip = c->e_skip();
    int b_skip = c->b_skip();

    if (e_skip == Cell::GLOBAL_ENV) {
      // Target environment is root envt.
      e = root_envt;
      e_count = e_skip;
    } else {
      // Skip the indicated number of environments.
      for (e_count = 0; (e != nil) && (e_count < e_skip); ++e_count)
        e = e->unsafe_cdr();
    }

    cellvector *v = e->unsafe_car()->VectorValue();

    if (b_skip < 0 || b_skip >= v->size() || e_skip != e_count) {
      printf("b=%d, e=%d, ec=%d, vs=%d\n", b_skip, e_skip, e_count, v->size());
      error("internal error: invalid lexical address: ", s->key);
    }
    // Go directly to the binding.
    return v->get(b_skip);
  }

  // Consider each environment in the enclosure chain
  // in turn, counting them as we go.

  for (e_count = 0; e != nil; ++e_count, e = e->unsafe_cdr()) {
    cellvector *v = e->unsafe_car()->VectorValue();
    // Check the current environment.

    for (b_count = 0; b_count < v->size(); ++b_count)
      if (v->get(b_count)->unsafe_car()->SymbolValue() == s) {
        if (e == root_envt) {
          // Top-level environment.  Due to nested defines,
          // this turns out to be a special case.
          e_count = -1;
        }

        quicken(c, e_count, b_count);
        return v->get(b_count);
      }
  }

  // Can the OS magically supply a value??

  if ((val = 0)) { // undef check
    // Yes! The OS has produced a value.  We cache
    // it in the outermost environment, as if it
    // had been established there with (define).

    Cell *os_binding = cons(c, val);
    car(root_envt)->VectorValue()->push(os_binding);
    return os_binding;
  }

  // Failure.
  return 0;
}

void Context::quicken(Cell *c, int e_count, int b_count) {
  // For global symbols, we have 16 bits of b_skip to work with;
  // only 8 if the symbol is not in the global environment.
  // XXX: these are magic numbers and should be coordinated with
  // the .h file.

  if (e_count >= 0) {
    if (e_count > 254 || b_count > 254)
      return;
  } else if (b_count > 65534)
    return;

  c->set_lexaddr(e_count, b_count);
}

Cell *Context::make_procedure(Cell *e, Cell *body, Cell *arglist) {
  cellvector *cv = cellvector::alloc(3);
  cv->set(0, e);
  cv->set(1, body);
  cv->set(2, arglist);
  Cell *c = alloc<Cell::Lambda>(cv);
  c->flag(Cell::Flag::VRef, true);

  return c;
}

Cell *Context::make_macro(Cell *e, Cell *body, Cell *arglist) {
  Cell *c = make_procedure(e, body, arglist);
  c->flag(Cell::Flag::Macro, true);
  return c;
}

Cell *Context::make_promise(Cell *e, Cell *body) {
  cellvector *cv = cellvector::alloc(1);
  Cell *c = alloc<Cell::Promise>(cv);
  c->flag(Cell::Flag::VRef, true);
  gc_protect(c);
  cv->set(0, make_procedure(e, body, nil));
  gc_unprotect();
  return c;
}

Cell *Context::make_list1(Cell *e1) { return cons(e1, nil); }

Cell *Context::make_list2(Cell *e1, Cell *e2) {
  return cons(e1, make_list1(e2));
}

Cell *Context::make_list3(Cell *e1, Cell *e2, Cell *e3) {
  return cons(e1, make_list2(e2, e3));
}

Cell *Context::make_continuation() {
  int msize = m_stack.size();
  cellvector *cv = cellvector::alloc(msize);
  for (int ix = 0; ix < msize; ++ix)
    cv->set(ix, m_stack[ix]);
  Cell *c = alloc<Cell::Cont>(cv);
  c->flag(Cell::Flag::VRef, true);

  return c;
}

void Context::load_continuation(Cell *cont) {
  cont->typecheck(Cell::Type::Cont);

  cellvector *cv = cont->unsafe_vector_value();
  int msize = cv->size();

  m_stack.clear();
  for (int ix = 0; ix < msize; ++ix)
    save(cv->get(ix));
}

class InterpreterExt : SchemeExtension {
public:
  InterpreterExt() { Register(this); }
  virtual void Install(Context *ctx, Cell *envt) {
    // Hook in the function pointer to the interpreter's evaluation loop.
    ctx->interp_eval = &Context::interp_evaluator;

    // Builtin Procedures (treated directly by `eval')

    static struct {
      const char *name; // name of procedure or form.
      bool macro;       // macro arguments are left unevaluated.
    } builtin[] = {
        {"and", true},
        {"apply", false},
        {"begin", true},
        {"call-with-current-continuation", false},
        {"call-with-input-file", false},
        {"call-with-output-file", false},
        {"case", true},
        {"cond", true},
        {"define", true},
        {"defmacro", true},
        {"delay", true},
        {"do", true},
        {"eval", false},
        {"for-each", false},
        {"force", false},
        {"if", true},
        {"lambda", true},
        {"let", true},
        {"let*", true},
        {"letrec", true},
        {"load", false},
        {"map", false},
        {"or", true},
        {"quasiquote", true},
        {"quote", true},
        {"set!", true},
        {"time", false},
        {"with-input-from-file", false},
        {"with-output-to-file", false},
    };

    for (unsigned int ix = 0; ix < sizeof(builtin) / sizeof(*builtin); ++ix) {
      psymbol ps = intern(builtin[ix].name);
      Cell *b = ctx->make_builtin(ps);
      ctx->set_var(envt, ps, b);
      if (builtin[ix].macro)
        b->flag(Cell::Flag::Macro, true);
    }
  }
};

static InterpreterExt interpreter_ext;
