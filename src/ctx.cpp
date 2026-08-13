//----------------------------------------------------------------------
// vx-scheme : Scheme interpreter.
// Copyright (c) 2002,2003,2006 and onwards Colin Smith.
//
// You may distribute under the terms of the Artistic License,
// as specified in the LICENSE file.
//
// ctx.cpp : Common material for a Scheme execution context, indpendent
// of whether the interpreter or the compiler is in use

#include "vx-scheme.h"

// --------------------------------------------------------------------------
// Initialize Static Data
//

const char *Cell::typeName[] = {
    "int",    "symbol", "unique", "string",   "real",    "subr", "lambda",
    "vector", "char",   "iport",  "oport",    "promise", "cont", "builtin",
    "magic",  "insn",   "cproc",  "cpromise", "cons"};

INTERN_SYM(s_unquote, "unquote");
INTERN_SYM(s_unquote_splicing, "unquote-splicing");
INTERN_SYM(s_dot, ".");
INTERN_SYM(s_quasiquote, "quasiquote");
INTERN_SYM(s_quote, "quote");

// --------------------------------------------------------------------------
// The Universal Cells
//

ALIGN8 Cell Cell::Nil;
ALIGN8 Cell Cell::Unspecified("#<unspecified>");
ALIGN8 Cell Cell::Unassigned("#<unassigned>");
ALIGN8 Cell Cell::Eof_Object("#<eof-object>");
ALIGN8 Cell Cell::Bool_T("#t");
ALIGN8 Cell Cell::Bool_F("#f");
ALIGN8 Cell Cell::Error("#<error>");
ALIGN8 Cell Cell::Halt("#<halt>");
ALIGN8 Cell Cell::Unimplemented("#<unimplemented>");

int Cell::typeCount[] = {0};
Cell *nil = &Cell::Nil;
Cell *unspecified = &Cell::Unspecified;
Cell *unassigned = &Cell::Unassigned;
Cell *unimplemented = &Cell::Unimplemented;

Context::Context() {
  // Conceivably, if the memory budget is very low, we could run
  // out while we're setting up all the builtin bindings.  We can't
  // GC, though, before the VM is set up.
  ok_to_gc = false;

  // Fresh environment.

  cellsAlloc = cellsTotal = 0;

  istack.push(make_iport(stdin));
  ostack.push(make_oport(stdout));

  envt = nil;
  // Clear out the function pointers that pertain to the interpreter
  // and bytecode VM; some of these will get filled in during the provision
  // step depending on which components are linked with the executable.
  vm_execute = 0;
  vm_eval = 0;
  interp_eval = 0;
  eval_cproc = 0;
  cc_procedure = 0;
  empty_vector = 0;

  root_envt = envt = extend(envt);

  provision();
  init_machine();
  ok_to_gc = true;
}

void Context::init_machine() {
  // Initialize machine registers

  r_exp = r_val = r_proc = r_unev = r_elt = r_nu = r_tmp = nil;
  r_env = envt;
  r_cproc = r_envt = nil;
  m_stack.clear();
  clear(r_argl);
  clear(r_varl);
}

char OS::errbuf[ebufsize];

// Context::using_vm - return true if we are using the bytecode vm.

bool Context::using_vm() const { return vm_eval && !interp_eval; }
// Context::eval
//   Switchyard for evaluator.  If the interpreter is present, we use
//   it (perhaps we're bootstrapping the compiler?)  Else we use the
//   bytecode virtual machine.

Cell *Context::eval(Cell *form) {
  if (using_vm())
    return (this->*vm_eval)(form);
  else if (interp_eval)
    return (this->*interp_eval)(form);
  error("no evaluator");
  return make_boolean(false);
}

void error(const char *message, const char *m2 /* = 0 */) {
  int ix = 0;
  const char *p;
  char *q;

  // Concatenate the two strings into a static buffer.

  for (p = message, ix = 0, q = OS::errbuf; *p && ix < OS::ebufsize - 1; ++ix)
    *q++ = *p++;

  if (m2)
    for (p = m2; *p && ix < OS::ebufsize - 1; ++ix)
      *q++ = *p++;

  *q = '\0';

  OS::exception();
}

Cell *Context::extend(Cell *env) {
  r_nu = make_vector(0);
  return make(r_nu, env);
}

// Context::find_var: find a variable in the given environment.  If
// index is not NULL, return the index of the variable (if found).  If
// the variable binding does not exist, NULL is returned and *index is
// unmolested.

Cell *Context::find_var(Cell *envt, psymbol var, unsigned int *index) {
  cellvector *bindings = car(envt)->VectorValue();
  for (int ix = 0; ix < bindings->size(); ++ix) {
    Cell *z = bindings->get(ix);
    if (car(z)->SymbolValue() == var) {
      if (index)
        *index = ix;
      return z;
    }
  }
  return 0;
}

void Context::set_var(Cell *envt, psymbol var, Cell *value,
                      unsigned int *index) {
  Cell *binding = find_var(envt, var, index);
  if (binding) {
    Cell::setcdr(binding, value);
    return;
  }
  // binding not found: add a new one
  binding = gc_protect(make_symbol(var));
  cellvector *v = car(envt)->VectorValue();
  v->push(cons(binding, gc_protect(value)));
  if (index)
    *index = v->size() - 1;
  gc_unprotect(2);
}

Cell *Context::RunMain() {
  if (SchemeExtension::HaveMain())
    return SchemeExtension::RunMain(this);

  return NULL;
}
