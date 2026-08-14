//----------------------------------------------------------------------
// vx-scheme : Scheme interpreter.
// Copyright (c) 2002,2003,2006 and onwards Colin Smith.
//
// You may distribute under the terms of the Artistic License,
// as specified in the LICENSE file.
//
// vm.cpp : PAIP-style virtual machine for compiled Scheme code
//

#include "vx-scheme.h"

enum operand_type { OP_NONE, OP_INT, OP_SYMBOL, OP_SUBR, OP_LEXADDR };

typedef struct {
  const char *name;
  enum operand_type opnd_type;
} vm_op;

// XXX issues:
// 1) the order of opcodes is willy-nilly.
// 2) there are magic number references to opcode numbers in this file.
//    Be careful.
// 3) I forget what (3) is.

static const vm_op optab[] = {
    // opcode number
    {"consti", OP_INT}, // 0
    {"nil", OP_NONE},        {"subr", OP_SUBR},
    {"gref", OP_SYMBOL},     {"gset", OP_SYMBOL},
    {"lref", OP_LEXADDR}, // 5
    {"lset", OP_LEXADDR},    {"goto", OP_INT},
    {"false?p", OP_INT},     {"false?", OP_INT},
    {"true?p", OP_INT}, // 10
    {"true?", OP_INT},       {"proc", OP_NONE},
    {"extend", OP_INT},      {"extend!", OP_NONE},
    {"extend.", OP_INT}, // 15
    {"save", OP_INT},        {"return", OP_NONE},
    {"pop", OP_NONE},        {"dup", OP_NONE},
    {"take", OP_INT}, // 20
    {"cc", OP_NONE},         {"resume", OP_NONE},
    {"apply.", OP_NONE},     {"apply", OP_INT},
    {"unspc", OP_NONE}, // 25
    {"unassn", OP_NONE},     {"lit", OP_INT},
    {"vector-set!", OP_INT}, // starting here:
    {"vector-ref", OP_INT},  // scheme primitives allocated to opcode
    {"car", OP_INT},         // 30
    {"cdr", OP_INT},         {"zero?", OP_INT},
    {"+", OP_INT},           {"*", OP_INT},
    {"quotient", OP_INT}, // 35
    {"remainder", OP_INT},   {"-", OP_INT},
    {"not", OP_INT},         {"null?", OP_INT},
    {"eq?", OP_INT}, // 40
    {"pair?", OP_INT},       {"cons", OP_INT},
    {"gref.", OP_INT},       {"false", OP_NONE},
    {"true", OP_NONE}, // 45
    {"int", OP_INT},         {"promise", OP_NONE},
    {"gset.", OP_INT},
};

static constexpr auto n_vmops = std::size(optab);

// exact_top_n: return true if the top n elements of the stack contained
// in cv are of exact type (in this implementation, exact is synonymous
// with integer).

static bool exact_top_n(cellvector *cv, int n) {
  int sz = cv->size();
  for (int ix = sz - n; ix < sz; ++ix) {
    if (!cv->get_unchecked(ix)->is<intptr_t>())
      return false;
  }
  return true;
}

// Context::extend
//   Extend an environment with the list of bindings in blist.

Cell *Context::extend(Cell *envt, Cell *blist) {
  Cell *xe = gc_protect(make_vector(0));
  cellvector *cv = xe->unsafe_vector_value();
  for (Cell *b = blist; b != nil; b = Cell::cdr(b))
    cv->push(car(b));
  envt = cons(xe, envt);
  gc_unprotect();
  return envt;
}

// Context::extend_from_vector
//   Extend environment envt with elements from the vector v, in
//   reverse order.  (The compiler arranges to compile function
//   arguments from left to right.  This means that the "rightmost"
//   argument to a function will be at the top of the stack.
//   References to parameters are by integer index, with the leftmost
//   argument numbered zero.)

Cell *Context::extend_from_vector(Cell *envt, cellvector *v, int n) {
  int size = v->size();
  r_nu = make_vector(n);
  cellvector *new_vec = r_nu->unsafe_vector_value();
  for (int ix = 0, iy = size - n; ix < n; ++ix, ++iy)
    new_vec->set_unchecked(ix, v->get_unchecked(iy));
  v->discard(n);
  envt = cons(r_nu, envt);
  return envt;
}

void Context::adjoin(Cell *envt, Cell *val) {
  car(envt)->unsafe_vector_value()->push(val);
}

// Context::pop_list
// Context::push_list
//   'pop' pops the specified number of elements off the machine stack and
//   returns a list of the elements.  The last element popped will be first
//   in the list.  'Push' pushes the supplied list onto the stack.  Elements
//   are pushed in the order given.

Cell *Context::pop_list(int n) {
  r_tmp = nil;
  for (int ix = 0; ix < n; ++ix) {
    r_tmp = cons(gc_protect(m_stack.pop()), r_tmp);
    gc_unprotect();
  }
  return r_tmp;
}

int Context::push_list(Cell *list) {
  int count = 0;
  for (Cell *a = list; a != nil; a = Cell::cdr(a)) {
    m_stack.push(car(a));
    ++count;
  }
  return count;
}

void Context::print_insn(int addr, const Cell *insn) {
  const Cell::Insn *iv = insn->InsnValue();
  const vm_op *op = optab + iv->opcode;
  printf("%4d:\t%s\t", addr, op->name);
  switch (op->opnd_type) {
  case OP_INT:
    printf("%ld", iv->int_val());
    break;
  case OP_SYMBOL:
    printf("%s", iv->Symbol()->key);
    break;
  case OP_SUBR:
    printf("%u,", iv->count);
    if (iv->is_quickened_subr())
      printf("%s", iv->subr_val()->name);
    else
      printf("%s", iv->Symbol()->key);
    break;
  case OP_LEXADDR: {
    auto la = iv->lex_addr();
    printf("%d,%d", la.e_skip, la.b_skip);
    break;
  }
  case OP_NONE:;
  }
  printf("\n");
}

// Context::vm_evaluator
//   Run the expression through the virtual machine's evaluator, if it's
//   present. (The evaluator is compiled code produced by the bootstrapper.)
//

Cell *Context::vm_evaluator(Cell *form) {
  if (!eval_cproc) {
    Cell *binding;
    if ((binding = find_var(root_envt, intern("eval"), 0)))
      eval_cproc = cdr(binding);
  }
  if (eval_cproc) {
    r_tmp = form;
    r_exp = cons(form, nil);
    //    save(r_envt);
    //    r_envt = root_envt;
    return execute(eval_cproc, r_exp);
    //    restore(r_envt);
  }
  error("can't find eval");
  return make_boolean(false);
}

Cell *Context::execute(Cell *proc, Cell *args) {
  cellvector *insns, *literals;
  intptr_t pc;
  intptr_t start;
  unsigned int count;
  unsigned int n_args = 0;
  unsigned int b_skip = 0;
  unsigned int e_skip = 0;

  // Note the initial stack size.
  int initial_stackdepth = m_stack.size();

  save_i(-1);

  // Push any arguments we received onto the stack.

  for (Cell *a = args; a != nil; a = Cell::cdr(a)) {
    ++n_args;
    save(car(a));
  }

  r_cproc = proc;
  bool trace = debug_flag(DebugFlag::TraceVm);
  bool trace_stack = debug_flag(DebugFlag::TraceVmStack);
  bool count_insns = debug_flag(DebugFlag::CountInsns);

  int xcount[n_vmops];
  if (count_insns)
    for (int ix = 0; ix < n_vmops; ++ix)
      xcount[ix] = 0;

  cellvector *root_bindings = car(root_envt)->unsafe_vector_value();

PROC:
  const cellvector *const v = r_cproc->unsafe_vector_value();
  insns = v->get(0)->unsafe_vector_value();
  literals = v->get(1)->unsafe_vector_value();
  r_envt = v->get(2);
  pc = v->get(3)->IntValue();

XEQ:
  Cell *insn = insns->get_unchecked(pc); // trust compiler!
  unsigned int opcode = insn->InsnValue()->opcode;
  if (count_insns)
    ++xcount[opcode];
  if (trace) {
    if (trace_stack) {
      printf("\t");
      for (int ix = m_stack.size() - 1; ix >= 0; --ix) {
        Cell *c = m_stack.get_unchecked(ix);
        if (!((reinterpret_cast<intptr_t>(c)) & 1)) {
          if (c == root_envt)
            printf("#<root-envt> ");
          else
            c->write(stdout);
        } else
          printf("%ld", (reinterpret_cast<intptr_t>(c)) >> 1);
        fputc(' ', stdout);
      }
      printf("\n");
    }
    print_insn(pc, insn);
  }
  switch (opcode) {
  case 0: // consti
    save_i(insn->InsnValue()->int_val());
    break;
  case 1: // nil
    m_stack.push(nil);
    break;
  case 2: // subr
    if (!insn->InsnValue()->is_quickened_subr()) {
      Cell *subr = find_var(root_envt, insn->InsnValue()->Symbol(), 0);
      if (!subr)
        error("missing primitive procedure");
      Cell *proc = cdr(subr);
      if (proc->is<Cell::Cproc>()) {
        // Yuck.  When the current procedure was compiled, the
        // routine we are about to invoke was a builtin (subr): now
        // it's a compiled procedure.  The optimized calling
        // convention for subrs no longer applies.  We must pop
        // the args off the stack, then push a continuation, then
        // re-push the args, and dispatch to the procedure.
        n_args = insn->InsnValue()->count;
        cellvector cv;
        for (unsigned int ix = 0; ix < n_args; ++ix)
          cv.push(m_stack.pop());
        save(r_envt);
        save(r_cproc);
        save_i(pc + 1);
        for (unsigned int ix = 0; ix < n_args; ++ix)
          m_stack.push(cv.pop());
        r_cproc = proc;
        goto PROC;
      } else if (auto *s = proc->get_if<Cell::Subr>()) {
        insn->InsnValue()->payload = s;
      } else {
        error("subr invoked on non-procedure");
      }
    }
    r_val = pop_list(insn->InsnValue()->count);
    // Subr's can change anything (in particular they can reenter execute).
    save(r_envt);
    save(r_cproc);
    r_val = insn->InsnValue()->subr_val()->subr(this, r_val);
    restore(r_cproc);
    restore(r_envt);
    m_stack.push(r_val);
    break;
  case 3: { // gref
    unsigned int index;
    r_val = find_var(root_envt, insn->InsnValue()->Symbol(), &index);
    if (!r_val) {
      error("reference to undefined global variable: ",
            insn->InsnValue()->Symbol()->key);
    } else {
      if (cdr(r_val) == nullptr)
        error("yikes"); // XXX
      // Quicken the instruction.
      insn->InsnValue()->opcode = 43; // gref.  XXX: magic number (among others)
      insn->InsnValue()->payload = static_cast<intptr_t>(index);
      m_stack.push(cdr(r_val));
    }
    break;
  }
  case 4: { // gset
    unsigned int index;
    set_var(root_envt, insn->InsnValue()->Symbol(), m_stack.pop(), &index);
    // Quicken the instruction.
    insn->InsnValue()->opcode = 48; // gset.  XXX: magic number
    insn->InsnValue()->payload = static_cast<intptr_t>(index);
    break;
  }
  case 5: { // lref
    auto la = insn->InsnValue()->lex_addr();
    e_skip = la.e_skip;
    b_skip = la.b_skip;
    r_tmp = r_envt;
    for (unsigned int ix = 0; ix < e_skip; ++ix)
      r_tmp = cdr(r_tmp);
    m_stack.push(car(r_tmp)->unsafe_vector_value()->get(b_skip));
    break;
  }
  case 6: { // lset
    auto la = insn->InsnValue()->lex_addr();
    e_skip = la.e_skip;
    b_skip = la.b_skip;
    r_tmp = r_envt;
    for (unsigned int ix = 0; ix < e_skip; ++ix)
      r_tmp = cdr(r_tmp);
    car(r_tmp)->unsafe_vector_value()->set(b_skip, m_stack.pop());
    break;
  }
  case 7: // goto
    pc = insn->InsnValue()->int_val();
    goto XEQ;
  case 8: // false?p
    if (!m_stack.pop()->istrue()) {
      pc = insn->InsnValue()->int_val();
      goto XEQ;
    }
    break;
  case 9: // false?
    if (!m_stack.top()->istrue()) {
      pc = insn->InsnValue()->int_val();
      goto XEQ;
    }
    break;
  case 10: // true?p
    if (m_stack.pop()->istrue()) {
      pc = insn->InsnValue()->int_val();
      goto XEQ;
    }
    break;
  case 11: // true?
    if (m_stack.top()->istrue()) {
      pc = insn->InsnValue()->int_val();
      goto XEQ;
    }
    break;
  case 12: // proc
    // pop the starting instruction from the stack and compose it
    // with the current environment.
    restore_i(start);
    m_stack.push(make_compiled_procedure(
        r_cproc->unsafe_vector_value()->get_unchecked(0),
        r_cproc->unsafe_vector_value()->get_unchecked(1), r_envt, start));
    break;
  case 13: // extend
    if (n_args < static_cast<unsigned int>(insn->InsnValue()->int_val()))
      error("vm: not enough arguments to procedure");
    r_envt = extend_from_vector(r_envt, &m_stack, insn->InsnValue()->int_val());
    break;
  case 14: // extend!
    r_envt = extend(r_envt, gc_protect(pop_list(1)));
    gc_unprotect();
    break;
  case 15: // extend.
    if (n_args < static_cast<unsigned int>(insn->InsnValue()->int_val()))
      error("vm: not enough arguments to procedure");
    r_val = pop_list(n_args - insn->InsnValue()->int_val());
    r_envt = extend(r_envt, gc_protect(pop_list(insn->InsnValue()->int_val())));
    gc_unprotect();
    adjoin(r_envt, r_val);
    break;
  case 16: // save
    // make a continuation that will invoke the indicated
    // instruction slot in this segment.
    save(r_envt);
    save(r_cproc);
    save_i(insn->InsnValue()->int_val());
    break;
  case 17:                 // return
    r_val = m_stack.pop(); // value
  RETURN:
    restore_i(pc);
    if (pc < 0)
      goto FINISH;
    restore(r_cproc);
    insns = r_cproc->unsafe_vector_value()->get(0)->VectorValue();
    literals = r_cproc->unsafe_vector_value()->get(1)->VectorValue();
    restore(r_envt);
    save(r_val);
    goto XEQ;
  case 18: // pop
    m_stack.pop();
    break;
  case 19: // dup
    m_stack.push(m_stack.top());
    break;
  case 20: { // take
    // Remove the n'th item from the stack and push it onto the top.
    // (We count from zero).  'take 0' would be a no-op; 'take 1'
    // would swap the top two elements.  We use an unchecked get
    // because we "trust the compiler."
    intptr_t target = insn->InsnValue()->int_val();
    int last = m_stack.size() - 1;
    r_tmp = m_stack.get_unchecked(last - target);
    for (int ix = last - target; ix < last; ++ix)
      m_stack.set(ix, m_stack.get_unchecked(ix + 1));
    m_stack.set(last, r_tmp);
    break;
  }
  case 21: { // cc
    r_tmp = make_vector(m_stack.size());
    cellvector *saved_stack = r_tmp->VectorValue();
    for (int ix = 0; ix < m_stack.size(); ++ix)
      saved_stack->set(ix, m_stack.get(ix));
    r_nu = cons(r_tmp, nil);
    r_envt = extend(r_envt, r_nu);
    m_stack.push(
        make_compiled_procedure(cc_procedure, empty_vector, r_envt, 0));
    r_envt = cdr(r_envt);
    // YYY
    break;
  }
  case 22: { // resume
    r_val = m_stack.pop();
    r_tmp = m_stack.pop();
    cellvector *new_stack = r_tmp->VectorValue();
    m_stack.clear(); // !
    for (int ix = 0; ix < new_stack->size(); ++ix)
      m_stack.push(new_stack->get(ix));
    goto RETURN;
  }
  case 23: // apply.
    // Covert stack from:   rest ... a2 a1 proc
    //                to:   proc a1 a2 ... rest
    // with 'rest' spliced in in the correct order.
    // Then do as in an ordinary apply.  This exists
    // only to support the 'apply' special procedure.
    r_tmp = m_stack.pop();
    for (count = 0; count < n_args - 2; ++count)
      r_tmp = cons(m_stack.pop(), r_tmp);
    r_proc = m_stack.pop();
    count = push_list(r_tmp);
    m_stack.push(r_proc);
    // dummy up the 'real' arument count that the
    // microcode for 'apply' will see below.
    insn->InsnValue()->payload = static_cast<intptr_t>(count);
  /* FALL THROUGH */
  case 24: // apply
    r_exp = m_stack.pop();
    {
      if (r_exp->is<Cell::Cproc>()) {
        n_args = insn->InsnValue()->int_val();
        r_cproc = r_exp;
        goto PROC;
      } else if (auto *s = r_exp->get_if<Cell::Subr>()) {
        r_val = pop_list(insn->InsnValue()->int_val());
        save(r_envt);
        save(r_cproc);
        r_val = s->subr(this, r_val);
        restore(r_cproc);
        restore(r_envt);
        goto RETURN;
      } else {
        r_exp->write(stderr);
        error("vm: inapplicable");
      }
    }
    break;
  case 25: // unspc
    m_stack.push(unspecified);
    break;
  case 26: // unassn
    m_stack.push(unassigned);
    break;
  case 27: // lit
    m_stack.push(literals->get(insn->InsnValue()->int_val()));
    break;
  case 28: { // vector-set!
    n_args = insn->InsnValue()->int_val();
    if (n_args != 3)
      error("bad arguments to vector-set!");
    int ix = m_stack.size() - 1;
    cellvector *cv = m_stack.get(ix - 2)->VectorValue();
    cv->set(m_stack.get(ix - 1)->IntValue(), m_stack.get(ix));
    m_stack.discard(3);
    m_stack.push(unspecified);
    break;
  }
  case 29: { // vector-ref
    n_args = insn->InsnValue()->int_val();
    if (n_args != 2)
      error("bad arguments to vector-ref!");
    intptr_t ix = m_stack.pop()->IntValue();
    cellvector *cv = m_stack.pop()->VectorValue();
    m_stack.push(cv->get(ix));
    break;
  }
  case 30: // car
    m_stack.push(car(m_stack.pop()));
    break;
  case 31: // cdr
    m_stack.push(cdr(m_stack.pop()));
    break;
  case 32: { // zero?
    Cell *c = m_stack.pop();
    if (c->is<intptr_t>())
      m_stack.push(make_boolean(c->IntValue() == 0));
    else if (c->is<double>())
      m_stack.push(make_boolean(c->RealValue() == 0.0));
    else
      error("non-numeric type");
    break;
  }
  case 33: { // +
    // get n; see if top n elements are all exact or not; add them
    // accumulating in situ (to avoid consing an argument list),
    // discard those elements and push the result.
    n_args = insn->InsnValue()->int_val();
    int sz = m_stack.size();
    if (exact_top_n(&m_stack, n_args)) {
      intptr_t sum = 0;
      for (int ix = sz - n_args; ix < sz; ++ix)
        sum += m_stack.get(ix)->IntValue(); // exact_top_n guarantees this is OK
      m_stack.discard(n_args);
      m_stack.push(make_int(sum));
    } else {
      double sum = 0.0;
      for (int ix = sz - n_args; ix < sz; ++ix)
        sum += m_stack.get(ix)->asReal();
      m_stack.discard(n_args);
      m_stack.push(make_real(sum));
    }
    break;
  }
  case 34: { // *
    // much like +, above.
    n_args = insn->InsnValue()->int_val();
    int sz = m_stack.size();
    if (exact_top_n(&m_stack, n_args)) {
      intptr_t product = 1;
      for (int ix = sz - n_args; ix < sz; ++ix)
        product *= m_stack.get(ix)->IntValue(); // exact_top_n says this is OK
      m_stack.discard(n_args);
      m_stack.push(make_int(product));
    } else {
      double product = 1.0;
      for (int ix = sz - n_args; ix < sz; ++ix)
        product *= m_stack.get(ix)->asReal();
      m_stack.discard(n_args);
      m_stack.push(make_real(product));
    }
    break;
  }
  case 35: { // quotient
    if (insn->InsnValue()->int_val() != 2)
      error("wrong # args");
    intptr_t d = m_stack.pop()->IntValue();
    intptr_t n = m_stack.pop()->IntValue();
    if (d == 0)
      error("/0");
    m_stack.push(make_int(n / d));
    break;
  }
  case 36: { // remainder
    if (insn->InsnValue()->int_val() != 2)
      error("wrong # args");
    intptr_t d = m_stack.pop()->IntValue();
    intptr_t n = m_stack.pop()->IntValue();
    if (d == 0)
      error("/0");
    m_stack.push(make_int(n % d));
    break;
  }
  case 37: { // -
    // get n; see if top n elements are all exact or not; add them
    // accumulating in situ (to avoid consing an argument list),
    // discard those elements and push the result.
    n_args = insn->InsnValue()->int_val();
    int sz = m_stack.size();
    if (exact_top_n(&m_stack, n_args)) {
      if (n_args == 1) {
        m_stack.push(make_int(-m_stack.pop()->IntValue()));
      } else {
        intptr_t difference = m_stack.get(sz - n_args)->IntValue();
        for (int ix = sz - n_args + 1; ix < sz; ++ix)
          difference -= m_stack.get(ix)->IntValue();
        m_stack.discard(n_args);
        m_stack.push(make_int(difference));
      }
    } else {
      if (n_args == 1) {
        m_stack.push(make_real(-m_stack.pop()->asReal()));
      } else {
        double difference = m_stack.get(sz - n_args)->asReal();
        for (int ix = sz - n_args + 1; ix < sz; ++ix)
          difference -= m_stack.get(ix)->asReal();
        m_stack.discard(n_args);
        m_stack.push(make_real(difference));
      }
    }
    break;
  }
  case 38: // not
    m_stack.push(m_stack.pop()->istrue() ? &Cell::Bool_F : &Cell::Bool_T);
    break;
  case 39: // null?
    m_stack.push(m_stack.pop() == &Cell::Nil ? &Cell::Bool_T : &Cell::Bool_F);
    break;
  case 40: // eq?
    m_stack.push(m_stack.pop()->eq(m_stack.pop()) ? &Cell::Bool_T
                                                  : &Cell::Bool_F);
    break;
  case 41: // pair?
    m_stack.push(m_stack.pop()->ispair() ? &Cell::Bool_T : &Cell::Bool_F);
    break;
  case 42: // cons (watch out: order matters, and cons can provoke GC.)
    r_tmp = m_stack.pop();
    r_elt = m_stack.pop();
    m_stack.push(cons(r_elt, r_tmp));
    break;
  case 43: { // gref. (quickened global ref; contains index of target binding)
    m_stack.push(cdr(root_bindings->get(insn->InsnValue()->int_val())));
    break;
  }
  case 44: // false
    m_stack.push(&Cell::Bool_F);
    break;
  case 45: // true
    m_stack.push(&Cell::Bool_T);
    break;
  case 46: // int
    m_stack.push(make_int(insn->InsnValue()->int_val()));
    break;
  case 47: // promise
    restore_i(start);
    r_tmp = make_compiled_procedure(r_cproc->unsafe_vector_value()->get(0),
                                    r_cproc->unsafe_vector_value()->get(1),
                                    r_envt, start);
    m_stack.push(make_compiled_promise(r_tmp));
    break;
  case 48: // gset.
    Cell::setcdr(root_bindings->get(insn->InsnValue()->int_val()),
                 m_stack.pop());
    break;
  default:
    error("unimplemented opcode_");
  }
  ++pc;
  goto XEQ;
FINISH:
  if (count_insns) {
    for (int ix = 0; ix < n_vmops; ++ix)
      printf("%s:%d ", optab[ix].name, xcount[ix]);
    printf("\n");
  }
  if (m_stack.size() != initial_stackdepth) {
    fprintf(stderr, "stack imbalance: %d (%d expected)\n", m_stack.size(),
            initial_stackdepth);
  }
  return r_val;
}

// find_op: match the supplied opcode symbol in the vm_op table;
// return the index (or -1 if the opcode is not in the table).

int find_op(psymbol opsym) {
  for (int ix = 0; ix < n_vmops; ++ix)
    if (!strcmp(optab[ix].name, opsym->key))
      return ix;
  return -1;
}

// Make compiled procedure (method and subr): store the
// current code segment, the environment, and program counter
// in an object.

static Cell *make_compiled_procedure(Context *ctx, Cell *arglist) {
  return ctx->make_compiled_procedure(car(arglist), cadr(arglist), nil, 0);
}

Cell *Context::make_compiled_procedure(Cell *insns, Cell *literals, Cell *envt,
                                       int start) {
  cellvector *cv = cellvector::alloc(4);
  cv->set(0, insns);
  cv->set(1, literals);
  cv->set(2, envt);
  cv->set(3, make_int(start));
  Cell *c = gc_protect(alloc<Cell::Cproc>(cv));
  c->flag(Cell::Flag::VRef, true);
  gc_unprotect();

  return c;
}

Cell *Context::make_compiled_promise(Cell *procedure) {
  cellvector *cv = cellvector::alloc(1);
  cv->set(0, procedure);
  Cell *c = gc_protect(alloc<Cell::Cpromise>(cv));
  c->flag(Cell::Flag::VRef, true);
  gc_unprotect();
  return c;
}

Cell *Context::force_compiled_promise(Cell *promise) {
  if (promise->flag(Cell::Flag::Forced))
    return promise->unsafe_vector_value()->get(0);
  Cell *val = execute(promise->unsafe_vector_value()->get(0), nil);
  // Did the promise become forced as a result of our evaluation?
  // then that value is correct.
  if (promise->flag(Cell::Flag::Forced))
    return promise->unsafe_vector_value()->get(0);
  promise->unsafe_vector_value()->set(0, val);
  promise->flag(Cell::Flag::Forced, true);
  return val;
}

// make_instruction: produce a packed machine instruction given
// an instruction in list form (e.g., '(consti 99) ).

static Cell *make_instruction(Context *ctx, Cell *arglist) {
  // return ctx->make_instruction (car (arglist));
  return ctx->make_instruction(arglist);
}

Cell *Context::make_instruction(Cell *insn) {
  psymbol op = car(insn)->SymbolValue();
  int opcode = find_op(op);
  if (opcode < 0)
    error("unknown opcode: ", op->key);
  return make_instruction(opcode, cdr(insn));
}

Cell *Context::make_instruction(int opcode, Cell *operands) {
  Cell *opnd = operands == nil ? nil : car(operands);
  Cell::Insn::Payload payload = std::monostate{};
  unsigned int count = 0;

  switch (optab[opcode].opnd_type) {
  case OP_INT:
    payload = opnd->IntValue();
    break;
  case OP_SYMBOL:
    payload = opnd->SymbolValue();
    break;
  case OP_SUBR: {
    int c = cadr(operands)->IntValue();
    if (c < 0 || c > 255)
      error("count too large to store in instruction field");
    count = c;
    payload = opnd->SymbolValue();
    break;
  }
  case OP_LEXADDR: {
    int u1 = opnd->IntValue();
    int u2 = cadr(operands)->IntValue();
    if (u1 > 32767 || u2 > 32767 || u1 < -32768 || u2 < -32768)
      error("lexical address too large");
    payload = Cell::LexAddr{static_cast<int16_t>(u1), static_cast<int16_t>(u2)};
    break;
  }
  case OP_NONE:
    break;
  default:
    error("unhandled operand type");
  }

  return alloc<Cell::Insn>(opcode, count, payload);
}

static Cell *execute(Context *ctx, Cell *arglist) {
  return ctx->execute(car(arglist), cdr(arglist));
}

static Cell *disassemble(Context *ctx, Cell *arglist) {
  cellvector *cproc = car(arglist)->CProcValue();
  cellvector *insns = cproc->get(0)->VectorValue();
  for (int ix = 0; ix < insns->size(); ++ix) {
    ctx->print_insn(ix, insns->get(ix));
  }
  return unspecified;
}

static Cell *write_compiled_procedure(Context *ctx, Cell *arglist) {
  return ctx->write_compiled_procedure(arglist);
}

// Context::load_compiled_procedure
//   Turn a serialized compiled procedure into a "live" procedure, by
//   reading the saved instructions and literals back into the Scheme
//   heap.
//   WARNING: This is expected to be called by the startup code with
//   GC disabled.

Cell *Context::load_compiled_procedure(vm_cproc *cp) {
  // We create a static argument list of two elements, which we reuse.
  Cell *insns = load_instructions(cp);
  Cell *literals = make_vector(cp->n_literals);
  cellvector *litv = literals->VectorValue();
  for (unsigned int ix = 0; ix < cp->n_literals; ++ix) {
    sstring litstr;
    litstr.append(cp->literals[ix]);
    Cell *lit = read(litstr);
    if (lit == nullptr)
      error("undecipherable literal", cp->literals[ix]);
    litv->set(ix, lit);
  }
  return make_compiled_procedure(insns, literals, nil, cp->entry);
}

Cell *Context::load_instructions(vm_cproc *cp) {
  Cell *zero = make_int(0);
  Cell *a1 = cons(zero, nil);
  Cell *a0 = cons(zero, a1); // now a0 == '(0 0)

  Cell *insns = make_vector(cp->n_insns);
  cellvector *insv = insns->VectorValue();
  for (unsigned int ix = 0; ix < cp->n_insns; ++ix) {
    vm_insn *insn = cp->insns + ix;
    int opcode = insn->opcode;
    if (opcode > n_vmops)
      error("bad opcode in stored proc");
    Cell::setcar(a0, zero);
    Cell::setcar(a1, zero);
    switch (optab[opcode].opnd_type) {
    case OP_INT:
      Cell::setcar(a0, make_int(reinterpret_cast<intptr_t>(insn->operand)));
      break;
    case OP_SYMBOL:
      Cell::setcar(
          a0, make_symbol(intern(static_cast<const char *>(insn->operand))));
      break;
    case OP_LEXADDR: {
      int la = reinterpret_cast<intptr_t>(insn->operand);
      Cell::setcar(a0, make_int(la >> 16));
      Cell::setcar(a1, make_int(la & 0xffff));
      break;
    }
    case OP_SUBR:
      Cell::setcar(
          a0, make_symbol(intern(static_cast<const char *>(insn->operand))));
      Cell::setcar(a1, make_int(insn->count));
      break;
    case OP_NONE:
      break;
    }
    insv->set(ix, make_instruction(opcode, a0));
  }
  return insns;
}

static void write_escaped_string(FILE *output, std::string_view str) {
  fputc('"', output);
  for (char c : str) {
    switch (c) {
    case '\n':
      fputc('\\', output);
      fputc('n', output);
      break;
    case '"':
    case '\\':
      fputc('\\', output);
      fputc(c, output);
      break;
    default:
      fputc(c, output);
    }
  }
  fputc('"', output);
}

Cell *Context::write_compiled_procedure(Cell *arglist) {
  cellvector *cproc = car(arglist)->CProcValue();
  const std::string &name = cadr(arglist)->StringValue();
  cellvector *insns = cproc->get(0)->VectorValue();
  cellvector *literals = cproc->get(1)->VectorValue();
  cellvector *root_bindings = car(root_envt)->VectorValue();
  int entry = cproc->get(3)->IntValue();
  FILE *output = current_output()->OportValue();
  fprintf(output, "static vm_insn %s_insns[] = {\n", name.c_str());
  for (int ix = 0; ix < insns->size(); ++ix) {
    const Cell *insn = insns->get(ix);
    const Cell::Insn *iv = insn->InsnValue();
    int opcode = iv->opcode;
    // Horrible special cases: 'gref./gset.'.  A "quickened global
    // reference" is an index into a slot in the global environment.
    // We can't write it out as is, since it's not likely that all
    // global variables will have the same slot in the context into
    // which this procedure will be loaded. Instead we write it out as
    // an ordinary 'gref', so that it can be quickened in the
    // environment in which it actually runs.
    if (opcode == 43) {                 // XXX magic number
      fprintf(output, "  { %2d,0,", 3); // XXX magic number
      write_escaped_string(
          output, car(root_bindings->get(iv->int_val()))->SymbolValue()->key);
    } else if (opcode == 48) {
      fprintf(output, "  { %2d,0,", 4); // XXX magic number
      write_escaped_string(
          output, car(root_bindings->get(iv->int_val()))->SymbolValue()->key);
    } else { // not 'gref.'
      const vm_op *op = optab + opcode;
      fprintf(output, "  { %2d,", opcode); // XXX magic number
      switch (op->opnd_type) {
      case OP_NONE:
        fprintf(output, "0,0");
        break;
      case OP_INT:
        fprintf(output, "0,(void*)%ld", iv->int_val());
        break;
      case OP_SYMBOL:
        fprintf(output, "0,");
        write_escaped_string(output, iv->Symbol()->key);
        break;
      case OP_SUBR:
        // XXX write a comment
        fprintf(output, "%u, ", iv->count);
        if (iv->is_quickened_subr())
          write_escaped_string(output, iv->subr_val()->name);
        else
          write_escaped_string(output, iv->Symbol()->key);
        break;
      case OP_LEXADDR: {
        auto la = iv->lex_addr();
        uint32_t packed_addr = (static_cast<uint16_t>(la.e_skip) << 16) |
                               static_cast<uint16_t>(la.b_skip);
        fprintf(output, "0,(void*)%#x", packed_addr);
        break;
      }
      }
    }
    fprintf(output, " },\n");
  }

  fprintf(output, "};\n\n");
  if (literals->size() > 0) {
    fprintf(output, "const char* %s_lit[] = {\n", name.c_str());
    for (int ix = 0; ix < literals->size(); ++ix) {
      sstring litstr;
      fputs("  ", output);
      literals->get(ix)->write(litstr);
      write_escaped_string(output, litstr.str());
      fputs(",\n", output);
    }
    fprintf(output, "};\n\n");
  }
  fprintf(output, "static vm_cproc %s = {\n  %s_insns,\n  %d,\n", name.c_str(),
          name.c_str(), insns->size());
  if (literals->size() > 0) {
    fprintf(output, "  %s_lit,\n  %d,\n", name.c_str(), literals->size());
  } else {
    fprintf(output, "  0,\n  0,\n");
  }
  fprintf(output, "  %d,\n", entry);
  fprintf(output, "};\n\n");

  return unspecified;
}

// ================================
// PROVISIONING THE VIRTUAL MACHINE
//

class VmExtension : SchemeExtension {
public:
  VmExtension() { Register(this); }
  virtual void Install(Context *ctx, Cell *envt) {
    static struct {
      const char *name;
      subr_f subr;
    } bindings[] = {
        {"make-instruction", make_instruction},
        {"make-compiled-procedure", make_compiled_procedure},
        {"write-compiled-procedure", write_compiled_procedure},
        {"disassemble", disassemble},
        {"execute", execute},
    };
    for (const auto &b : bindings) {
      ctx->bind_subr(b.name, b.subr);
    }
    // Initialize the macro table.
    ctx->set_var(envt, intern("__macro_table"), nil);
    // Attach VM execution function to context, so the interpreter may
    // invoke compiled procedures.
    ctx->vm_execute = &Context::execute;
    ctx->vm_eval = &Context::vm_evaluator;
  }
};

static VmExtension vm_extension;
