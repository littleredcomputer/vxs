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
  ok_to_gc = true;
}

void Context::unregister_fiber(Fiber *f) {
  auto it = std::find(active_fibers.begin(), active_fibers.end(), f);
  if (it != active_fibers.end())
    active_fibers.erase(it);
}

std::unique_ptr<Fiber> Context::spawn_fiber(Cell *form) {
  return std::make_unique<Fiber>(*this, form);
}

__attribute__((weak)) Step Context::eval_coro(Fiber &f) {
  co_return;
}

__attribute__((weak)) Step Context::vm_coro(Fiber &f, Cell *proc, Cell *args) {
  co_return;
}

Fiber::Fiber(Context &c, Cell *form) : ctx(c) {
  r_exp = form;
  r_env = c.root();
  r_val = r_proc = r_unev = r_elt = r_nu = r_tmp = nil;
  r_envt = nil;
  r_cproc = nil;
  r_qq = 0;
  r_cont = 1;
  state = 0;
  clear(r_argl);
  clear(r_varl);
  c.register_fiber(this);
  step = c.eval_coro(*this);
}

Fiber::Fiber(Context &c, Cell *proc, Cell *args) : ctx(c) {
  r_cproc = proc;
  r_exp = args;
  r_env = c.root();
  r_val = r_proc = r_unev = r_elt = r_nu = r_tmp = nil;
  r_envt = nil;
  r_qq = 0;
  r_cont = 1;
  state = 0;
  clear(r_argl);
  clear(r_varl);
  c.register_fiber(this);
  step = c.vm_coro(*this, proc, args);
}

Fiber::~Fiber() { ctx.unregister_fiber(this); }

bool Fiber::next() {
  Fiber *saved = ctx.current_fiber;
  ctx.current_fiber = this;
  bool running = step.next();
  ctx.current_fiber = saved;
  if (!running) {
    if (m_future && m_future->is<Cell::Future>()) {
      auto &fut = m_future->as<Cell::Future>();
      fut.completed = true;
      fut.result = r_val ? r_val : &Cell::Unspecified;
    }
    ctx.unregister_fiber(this);
  }
  return running;
}

Cell *Fiber::pop_list(int n) {
  r_tmp = nil;
  for (int ix = 0; ix < n; ++ix) {
    r_tmp = ctx.cons(ctx.gc_protect(m_stack.pop()), r_tmp);
    ctx.gc_unprotect();
  }
  return r_tmp;
}

int Fiber::push_list(Cell *list) {
  int count = 0;
  for (Cell *a = list; a != nil; a = Cell::cdr(a)) {
    m_stack.push(car(a));
    ++count;
  }
  return count;
}

void Fiber::l_append(Cell &l, Cell *t) {
  r_elt = ctx.make(t);
  l_appendtail(l, r_elt);
}

void Fiber::mark_roots(Context *gc) {
  gc->mark(r_env);
  gc->mark(r_envt);
  gc->mark(r_cproc);
  gc->mark(Cell::car(&r_argl));
  gc->mark(Cell::cdr(&r_argl));
  gc->mark(Cell::car(&r_varl));
  gc->mark(Cell::cdr(&r_varl));
  gc->mark(r_proc);
  gc->mark(r_exp);
  gc->mark(r_unev);
  gc->mark(r_val);
  gc->mark(r_tmp);
  gc->mark(r_elt);
  gc->mark(r_nu);
  gc->mark(m_future);

  for (int ix = 0; ix < m_stack.size(); ++ix) {
    Cell *p = m_stack[ix];
    if ((reinterpret_cast<intptr_t>(p) & Cell::ATOM) == 0)
      gc->mark(p);
  }
}

void Fiber::print_vm_state() const {
  printf("%d state=%d exp=", m_stack.size(), state);
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
  if (r_env == ctx.root())
    printf("#<root>");
  else
    Cell::car(r_env)->write(stdout);
  printf(" cont=%ld q%d\n", r_cont, r_qq);
}

Cell *Fiber::make_continuation() {
  int msize = m_stack.size();
  cellvector *cv = cellvector::alloc(msize);
  for (int ix = 0; ix < msize; ++ix)
    cv->set(ix, m_stack[ix]);
  Cell *c = ctx.alloc<Cell::Cont>(cv);
  c->flag(Cell::Flag::VRef, true);
  return c;
}

void Fiber::load_continuation(Cell *cont) {
  cellvector *cv = cont->unsafe_vector_value();
  int msize = cv->size();

  m_stack.clear();
  for (int ix = 0; ix < msize; ++ix)
    push(cv->get(ix));
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

void error(std::string_view message, std::string_view m2) {
  std::string err(message);
  if (!m2.empty()) {
    err += m2;
  }
  throw std::runtime_error(err);
}

Cell *Context::extend(Cell *env) {
  Cell *v = gc_protect(make_vector(0));
  Cell *res = make(v, env);
  gc_unprotect();
  return res;
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

bool Context::step_fibers() {
  auto fibers = active_fibers;
  for (Fiber *fib : fibers) {
    if (fib == current_fiber || !fib->future()) {
      continue;
    }
    if (std::find(active_fibers.begin(), active_fibers.end(), fib) != active_fibers.end()) {
      fib->next();
    }
  }
  for (Fiber *fib : active_fibers) {
    if (fib != current_fiber && fib->future())
      return true;
  }
  return false;
}

void Context::run_fibers() {
  while (true) {
    bool has_other = false;
    for (Fiber *fib : active_fibers) {
      if (fib != current_fiber && fib->future()) {
        has_other = true;
        break;
      }
    }
    if (!has_other)
      break;
    step_fibers();
  }
}

Cell *Context::touch_future(Cell *fut) {
  if (!fut || !fut->is<Cell::Future>()) {
    error("touch: expected a future");
  }
  auto &f = fut->as<Cell::Future>();
  if (f.completed) {
    return f.result ? f.result : &Cell::Unspecified;
  }
  while (!f.completed) {
    bool other_active = false;
    for (Fiber *fib : active_fibers) {
      if (fib != current_fiber && fib->future()) {
        other_active = true;
        break;
      }
    }
    if (!other_active) {
      break;
    }
    step_fibers();
  }
  return f.completed ? (f.result ? f.result : &Cell::Unspecified) : &Cell::Unspecified;
}
