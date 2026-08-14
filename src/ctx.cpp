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



psymbol s_unquote = intern("unquote");
psymbol s_unquote_splicing = intern("unquote-splicing");
psymbol s_dot = intern(".");
psymbol s_quasiquote = intern("quasiquote");
psymbol s_quote = intern("quote");

// --------------------------------------------------------------------------
// The Universal Cells
//

Cell Cell::Nil;
Cell Cell::Unspecified("#<unspecified>");
Cell Cell::Unassigned("#<unassigned>");
Cell Cell::Eof_Object("#<eof-object>");
Cell Cell::Bool_T("#t");
Cell Cell::Bool_F("#f");
Cell Cell::Error("#<error>");
Cell Cell::Halt("#<halt>");
Cell Cell::Unimplemented("#<unimplemented>");

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

#include <stdexcept>
#include <string>

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
  std::string err = message;
  if (m2) {
    err += m2;
  }
  throw std::runtime_error(err);
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

  return nullptr;
}
