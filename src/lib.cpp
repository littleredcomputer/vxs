//----------------------------------------------------------------------
// vx-scheme : Scheme interpreter.
// Copyright (c) 2002,2003,2006 and onwards Colin Smith.
//
// You may distribute under the terms of the Artistic License,
// as specified in the LICENSE file.
//
// lib.cpp : A few extra library functions used in the compiled-code VM
//

#include "vx-scheme.h"

static Cell *force(Context *ctx, Cell *arglist) {
  return ctx->force_compiled_promise(car(arglist));
}

// XXX: I'm not sure if the following two interact correctly
// with call-with-current-continuation.  They should probably
// become opcodes (sigh)

static Cell *with_input_from_file(Context *ctx, Cell *arglist) {
  ctx->with_input(car(arglist)->StringValue());
  Cell *val = ctx->execute(cadr(arglist), nil);
  ctx->without_input();
  return val;
}

static Cell *with_output_to_file(Context *ctx, Cell *arglist) {
  ctx->with_output(car(arglist)->StringValue());
  Cell *val = ctx->execute(cadr(arglist), nil);
  ctx->without_output();
  return val;
}

static Cell *time(Context *ctx, Cell *arglist) {
  double t0 = vx_get_time();
  Cell *val = ctx->execute(car(arglist), nil);
  double t1 = vx_get_time();
  ctx->gc_protect(val);
  Cell *d = ctx->make_real(t1 - t0);
  ctx->gc_protect(d);
  Cell *res = ctx->cons(d, val);
  ctx->gc_unprotect(2);
  return res;
}

// When call-with-current-continuation is used, the value supplied
// is in the form of a procedure which when invoked will resume
// the computation at the correct point.  This is the body of that
// procedure, written here in "assembly language."  (We can't write
// it in scheme because the resume instruction is not reachable from
// there.)

static vm_insn _callcc_procedure_insns[] = {
    {13, 0, (void *)1},      // extend 1         XXX magic number
    {5, 0, (void *)0x10000}, // lref 1,0               "     "
    {5, 0, 0x0},             // lref 0,0               "     "
    {22, 0, 0},              // resume                 "     "
};

static vm_cproc _callcc_procedure = {
    _callcc_procedure_insns,
    static_cast<unsigned int>(std::size(_callcc_procedure_insns)),
    0, // literals
    0, // # literals
    0, // starting insn
};

class VmLibExtension : SchemeExtension {
public:
  VmLibExtension() { Register(this); }
  virtual void Install(Context *ctx, Cell *envt) {
    static struct {
      const char *name;
      subr_f subr;
    } bindings[] = {
        {"force", force},
        {"with-output-to-file", with_output_to_file},
        {"with-input-from-file", with_input_from_file},
        {"time", time},
    };
    for (const auto &b : bindings) {
      ctx->bind_subr(b.name, b.subr);
    }
    // Compile the procedure stub for a saved continuation
    ctx->cc_procedure = ctx->load_instructions(&_callcc_procedure);
    ctx->empty_vector = ctx->make_vector(0);
    ctx->set_var(envt, intern("__callcc_procedure"), ctx->cc_procedure);
  }
};

static VmLibExtension vm_lib_extension;
