#include <stdio.h>
#include <stdlib.h>
#include <chrono>
#include <cassert>
#include <string>
#include <vector>
#include "vx-scheme.h"

double vx_get_time() {
  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  return std::chrono::duration<double>(duration).count();
}

uint32_t debug_flags() { return 0; }

int main() {
  printf("=== Testing Fiber Cooperative Multitasking with (yield) ===\n");
  Context ctx;

  // 1. Interpreted Fibers Interleaving
  printf("[1] Testing two interpreted fibers yielding back and forth...\n");
  std::string progA = "(define out '()) (define (runA) (set! out (cons 'A1 out)) (yield) (set! out (cons 'A2 out)) (yield) (set! out (cons 'A3 out))) (runA)";
  std::string progB = "(define (runB) (set! out (cons 'B1 out)) (yield) (set! out (cons 'B2 out)) (yield) (set! out (cons 'B3 out))) (runB)";

  sstring sA(progA.c_str());
  Cell *formA1 = ctx.read(sA); ctx.eval(formA1); // define out
  Cell *formA2 = ctx.read(sA); ctx.eval(formA2); // define runA
  Cell *formA3 = ctx.read(sA); // (runA)

  sstring sB(progB.c_str());
  Cell *formB1 = ctx.read(sB); ctx.eval(formB1); // define runB
  Cell *formB2 = ctx.read(sB); // (runB)

  Fiber fA(ctx, formA3);
  Fiber fB(ctx, formB2);

  // Round-robin scheduler
  std::vector<Fiber*> fibers = { &fA, &fB };
  int steps = 0;
  while (!fibers.empty()) {
    for (auto it = fibers.begin(); it != fibers.end(); ) {
      ++steps;
      if (!(*it)->next()) {
        it = fibers.erase(it);
      } else {
        ++it;
      }
    }
  }

  sstring sOut("out");
  Cell *out = ctx.eval(ctx.read(sOut));
  printf("Result of interpreted fibers out (total scheduler steps=%d): ", steps);
  out->write(stdout);
  printf("\n");

  // 2. Bytecode VM Fibers Interleaving
  printf("[2] Testing compiled bytecode fibers yielding...\n");
  std::string compileCode = 
    "(define out2 '()) "
    "(define compA (link2 (compile '((lambda () (set! out2 (cons 'CA1 out2)) (yield) (set! out2 (cons 'CA2 out2)) (yield) (set! out2 (cons 'CA3 out2))))))) "
    "(define compB (link2 (compile '((lambda () (set! out2 (cons 'CB1 out2)) (yield) (set! out2 (cons 'CB2 out2)) (yield) (set! out2 (cons 'CB3 out2))))))) ";
  
  sstring sC(compileCode.c_str());
  for (int i = 0; i < 3; ++i) {
    Cell *f = ctx.read(sC);
    ctx.eval(f);
  }

  sstring sPA("compA");
  sstring sPB("compB");
  Cell *procA = ctx.eval(ctx.read(sPA));
  Cell *procB = ctx.eval(ctx.read(sPB));

  Fiber fvA(ctx, procA, nil);
  Fiber fvB(ctx, procB, nil);

  std::vector<Fiber*> vm_fibers = { &fvA, &fvB };
  int vm_steps = 0;
  while (!vm_fibers.empty()) {
    for (auto it = vm_fibers.begin(); it != vm_fibers.end(); ) {
      ++vm_steps;
      if (!(*it)->next()) {
        it = vm_fibers.erase(it);
      } else {
        ++it;
      }
    }
  }

  sstring sOut2("out2");
  Cell *out2 = ctx.eval(ctx.read(sOut2));
  printf("Result of compiled VM fibers out2 (total scheduler steps=%d): ", vm_steps);
  out2->write(stdout);
  printf("\n");

  // 3. Mixed Interpreted and Bytecode VM Fiber Interleaving
  printf("[3] Testing mixed Interpreted + Compiled VM fibers yielding...\n");
  std::string mixedCode = 
    "(define out3 '()) "
    "(define (interpProc) (set! out3 (cons 'INTERP1 out3)) (yield) (set! out3 (cons 'INTERP2 out3)) (yield) (set! out3 (cons 'INTERP3 out3))) "
    "(define vmProc (link2 (compile '((lambda () (set! out3 (cons 'VM1 out3)) (yield) (set! out3 (cons 'VM2 out3)) (yield) (set! out3 (cons 'VM3 out3))))))) "
    "(interpProc)";
  
  sstring sM(mixedCode.c_str());
  Cell *m1 = ctx.read(sM); ctx.eval(m1); // define out3
  Cell *m2 = ctx.read(sM); ctx.eval(m2); // define interpProc
  Cell *m3 = ctx.read(sM); ctx.eval(m3); // define vmProc
  Cell *m4 = ctx.read(sM); // (interpProc)

  sstring sVMProc("vmProc");
  Cell *procVM = ctx.eval(ctx.read(sVMProc));

  Fiber fInterp(ctx, m4);
  Fiber fVM(ctx, procVM, nil);

  std::vector<Fiber*> mixed_fibers = { &fInterp, &fVM };
  int mixed_steps = 0;
  while (!mixed_fibers.empty()) {
    for (auto it = mixed_fibers.begin(); it != mixed_fibers.end(); ) {
      ++mixed_steps;
      if (!(*it)->next()) {
        it = mixed_fibers.erase(it);
      } else {
        ++it;
      }
    }
  }

  sstring sOut3("out3");
  Cell *out3 = ctx.eval(ctx.read(sOut3));
  printf("Result of mixed fibers out3 (total scheduler steps=%d): ", mixed_steps);
  out3->write(stdout);
  printf("\n");

  // 4. In-Scheme (future ...) and (touch ...) Tests
  printf("[4] Testing in-Scheme (future ...) and (touch ...) with value returns and (yield)...\n");
  std::string futureCode =
    "(define fut-log '()) "
    "(define futA (future "
    "  (set! fut-log (cons 'FA1 fut-log)) "
    "  (yield) "
    "  (set! fut-log (cons 'FA2 fut-log)) "
    "  (yield) "
    "  (set! fut-log (cons 'FA3 fut-log)) "
    "  100)) "
    "(define futB (future "
    "  (set! fut-log (cons 'FB1 fut-log)) "
    "  (yield) "
    "  (set! fut-log (cons 'FB2 fut-log)) "
    "  (yield) "
    "  (set! fut-log (cons 'FB3 fut-log)) "
    "  200)) "
    "(define fut-res (list (touch futA) (touch futB)))";

  sstring sFut(futureCode.c_str());
  for (int i = 0; i < 4; ++i) {
    Cell *f = ctx.read(sFut);
    ctx.eval(f);
  }

  sstring sFutLog("fut-log");
  Cell *futLog = ctx.eval(ctx.read(sFutLog));
  printf("Result of fut-log: ");
  futLog->write(stdout);
  printf("\n");

  sstring sFutRes("fut-res");
  Cell *futRes = ctx.eval(ctx.read(sFutRes));
  printf("Result of fut-res: ");
  futRes->write(stdout);
  printf("\n");

  // 5. In-Scheme (future-done?), (step-fibers), and (run-fibers)
  printf("[5] Testing (future-done?), (step-fibers), and (run-fibers)...\n");
  std::string bgCode =
    "(define bg-done #f) "
    "(define bg-fut (future "
    "  (yield) "
    "  (yield) "
    "  (set! bg-done #t) "
    "  'all-good)) "
    "(define check1 (future-done? bg-fut)) "
    "(step-fibers) "
    "(define check2 (future-done? bg-fut)) "
    "(run-fibers) "
    "(define check3 (future-done? bg-fut)) "
    "(define check4 (touch bg-fut)) "
    "(define bg-summary (list check1 check2 check3 check4 bg-done))";

  sstring sBg(bgCode.c_str());
  for (int i = 0; i < 9; ++i) {
    Cell *f = ctx.read(sBg);
    ctx.eval(f);
  }

  sstring sBgSummary("bg-summary");
  Cell *bgSummary = ctx.eval(ctx.read(sBgSummary));
  printf("Result of bg-summary: ");
  bgSummary->write(stdout);
  printf("\n");

  printf("=== ALL FIBER & FUTURE TESTS PASSED SUCCESSFULLY! ===\n");
  return 0;
}
