//----------------------------------------------------------------------
// vx-scheme : Scheme interpreter.
// Copyright (c) 2002,2003,2006 and onwards Colin Smith.
//
// You may distribute under the terms of the Artistic License,
// as specified in the LICENSE file.
//
// main.cpp : startup code for UNIX environments.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <chrono>
#include <stdexcept>
#include "vx-scheme.h"

//----------------------------------------------------------------------------
//
// SYSTEM ABSTRACTION
//
//----------------------------------------------------------------------------

double vx_get_time() {
  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  return std::chrono::duration<double>(duration).count();
}

uint32_t debug_flags() {
  static bool env_checked = false;
  static uint32_t f = 0;
  if (!env_checked) {
    if (const char *c = getenv("T"))
      f = strtoul(c, nullptr, 0);
    env_checked = true;
  }
  return f;
}

void interact(Context *ctx) {
  bool interactive = (isatty(0) != 0);

  while (ctx->read_eval_print(stdin, stdout, interactive))
    ;

  if (debug_flag(DebugFlag::MemstatsAtExit)) {
    ctx->print_mem_stats(stdout);
  }

  exit(0);
}

int main(int argc, char **argv) {
  Context ctx;
  Cell *scheme_argv = ctx.gc_protect(ctx.make_vector(0));
  cellvector *argvec = scheme_argv->VectorValue();

  --argc;
  ++argv;

  while (argc > 0) {
    argvec->push(ctx.make_string(*argv));
    --argc;
    ++argv;
  }

  // Establish *argv* in global environment

  ctx.set_var(intern("*argv*"), scheme_argv, 0);
  ctx.gc_unprotect();

  // See if we have a canned main procedure.

  Cell *result = ctx.RunMain();
  if (result) {
    if (result != unspecified)
      result->write(stdout);
  } else {
    // Interact

    while (1) {
      try {
        interact(&ctx);
      } catch (const std::exception &e) {
        fprintf(stderr, "caught exception: %s\n", e.what());
      }
    }
  }
}
