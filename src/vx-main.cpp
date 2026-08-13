//----------------------------------------------------------------------
// vx-scheme : Scheme interpreter.
// Copyright (c) 2002,2003,2006 and onwards Colin Smith.
//
// You may distribute under the terms of the Artistic License,
// as specified in the LICENSE file.
//
// vx-main.cpp : startup code for VxWorks execution environment.

#include "vx-scheme.h"
#include "tickLib.h"
#include "sysSymTbl.h"
#include "setjmp.h"

static jmp_buf jb;
static psymbol s_vx_invoke;
static psymbol s_args;
int vxSchemeDebug = 0;

extern "C" int sysClkRateGet();
typedef int (*VX_FUNC)(...);

//----------------------------------------------------------------------------
//
// OS-SPECIFIC FEATURES
//
// This area fills in definitions for OS-specific features named
// in class OS.
//

double OS::get_time() {
  double t = tickGet();
  return t / sysClkRateGet();
}

unsigned int OS::flags() { return vxSchemeDebug; }

bool OS::interactive(int fd) { return isatty(fd); }

Cell *mget(Context *ctx, void *key) {
  int *pi = (int *)key;
  int val = *pi;
  return ctx->make_int(val);
}

void mset(Context *ctx, void *key, Cell *rhs) {
  int value;
  Cell::Type t = rhs->type();

  if (t == Cell::Int)
    value = rhs->IntValue();
  else if (t == Cell::String)
    value = reinterpret_cast<int>(rhs->StringValue());
  else if (t == Cell::Char)
    value = rhs->CharValue();
  else
    error("cannot convert rvalue to compatible type");

  int *pi = static_cast<int *>(key);
  *pi = value;
}

Cell *OS::undef(Context *ctx, const char *name) {
  char *value;
  SYM_TYPE type;

  // See if it's a symbol.

  if (symFindByCName(sysSymTbl, const_cast<char *>(name), &value, &type) ==
      OK) {
    switch (type) {
    case SYM_GLOBAL | SYM_BSS:
    case SYM_GLOBAL | SYM_DATA:

      // It's a global in a data section.  Treat
      // it as an integer variable.

      return ctx->make_magic(value, mset, mget);

    case SYM_GLOBAL | SYM_TEXT:

      // It's a global in a text section.  Treat
      // it as a function.  To do this, we construct
      // a lambda which will call the VxWorks
      // function below:
      //
      // (lambda args (vx-invoke <function-address> args))

      Cell *vx_invoke, *addr, *args, *nu;

      vx_invoke = ctx->make_symbol(s_vx_invoke);
      ctx->gc_protect(vx_invoke);
      addr = ctx->make_int(reinterpret_cast<int>(value));
      ctx->gc_protect(addr);
      args = ctx->make_symbol(s_args);
      ctx->gc_protect(args);
      nu = ctx->make_list3(vx_invoke, addr, args);
      ctx->gc_protect(nu);
      nu = ctx->cons(nu, nil);
      ctx->gc_unprotect(4);
      return ctx->make_procedure(ctx->root(), nu, args);
    }
  }
  return 0;
}

Cell *vx_invoke(Context *ctx, Cell *arglist) {
  Cell *cfunc = car(arglist);
  Cell *alist = cadr(arglist);
  const int nargs = 10;
  int a[nargs];
  int ix = 0;
  VX_FUNC vx_func = reinterpret_cast<VX_FUNC>(cfunc->IntValue());

  // Fill up argument array.  We support integer and string
  // arguments (which we pass by address).   If we see a
  // symbol (most likely someone wrote, e.g., 'taskDelay),
  // we look up its value in the VxWorks symbol table.  This
  // makes "(sp 'taskDelay 100)" work (if the quote were omitted,
  // then taskDelay would receive a procedure value, rather than
  // a numeric one).

  FOR_EACH(arg, alist) {
    Cell *ar = car(arg);
    Cell::Type t = ar->type();
    if (t == Cell::Int)
      a[ix++] = ar->IntValue();
    else if (t == Cell::String)
      a[ix++] = reinterpret_cast<int>(ar->StringValue());
    else if (t == Cell::Symbol) {
      const char *name = ar->SymbolValue()->truename;
      char *value;
      SYM_TYPE type;

      if (symFindByCName(sysSymTbl, const_cast<char *>(name), &value, &type) ==
          OK)
        a[ix++] = reinterpret_cast<int>(value);
      else
        error("symbol absent from sysSymTbl");
    } else
      error("incompatible argument type");
  }

  // Fill up the remaining argument slots with '0'.

  for (; ix < nargs; ++ix)
    a[ix] = 0;

  // Invoke VxWorks function.  Make an integer cell of the
  // return value.

  return ctx->make_int(
      vx_func(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]));
}

void OS::exception(const char *s) { longjmp(jb, 1); }

void interact(Context *ctx) {
  bool interactive = isatty(0);

  while (ctx->read_eval_print(stdin, stdout, interactive))
    ;

  if (OS::flag(DEBUG_MEMSTATS_AT_EXIT))
    ctx->print_mem_stats(stdout);

  exit(0);
}

extern "C" int scheme(char *a0) {
  Context ctx;

  // Sanity check: we need to make sure that the "unique cells"
  // (e.g., things like nil, etc.) are 8-byte aligned.  If this
  // scheme image has been dynamically loaded to a VxWorks system,
  // this is not easy to guarantee!  We try to favor this outcome by
  // making env.o (where these objects are defined) first in the
  // link order, but we make sure that whatever happens things have
  // worked out ok.  The garbage collector will be very unhappy if
  // any cells are not 8-aligned.

  if ((reinterpret_cast<intptr_t>(nil)) & 7) {
    printf("code module error: standard cells not 8-aligned\n");
    exit(1);
  }

  s_args = intern("args");
  s_vx_invoke = intern("vx-invoke");

  ctx.bind(ctx.make_symbol(intern("vx-invoke")),
           ctx.make_subr(vx_invoke, "vx-invoke"));

  if (a0 != 0) {
    sstring ss;
    ss.append(a0);
    Cell *result = ctx.eval(ctx.read(ss));
    if (result != unspecified) {
      result->write(stdout);
      fputc('\n', stdout);
    }
  } else
    while (1) {
      if (setjmp(jb) == 0)
        interact(&ctx);
      else
        fprintf(stderr, "caught: %s\n", OS::errbuf);
    }
}
