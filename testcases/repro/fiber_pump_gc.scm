;;; REPRODUCTION: a nested future/touch segfaults under --gc-stress.
;;;
;;;   ./src/vx-scheme --gc-stress testcases/repro/fiber_pump_gc.scm
;;;   -> exit 139 (SIGSEGV), no output
;;;
;;; Clean without the flag, and clean at every --gc-threshold: it needs a
;;; collection landing inside the nested scheduler round, which only
;;; collecting on EVERY allocation makes certain.
;;;
;;; WHERE IT DIES. lldb puts it in VM::run_dispatch on
;;;
;;;   ldp x8, x10, [x8, #0x18]     with x8 = 0
;;;
;;; which is `frame->closure->chunk` read through a null. run_dispatch
;;; holds `frame`, `ip` and `chunk` as raw locals; `frames` is a deque and
;;; the stack is a SlabStack, so neither RELOCATES -- but a collected
;;; closure leaves `chunk` dangling all the same. So a frame's closure was
;;; freed while its fiber was still running.
;;;
;;; WHAT THAT IMPLIES. mark_roots reaches a fiber two ways only:
;;; active_fibers, and the current_fiber -> parent_fiber chain. OP_TOUCH's
;;; rescue path (vx_vm.cpp, "Pump the whole scheduler") runs
;;; step_all_active_fibers from inside the touching fiber's own step, and
;;; that sets current_fiber to each candidate in turn. Something in that
;;; window is reachable from neither route.
;;;
;;; NOT YET DIAGNOSED FURTHER, deliberately. Two confident guesses were
;;; already wrong today (see MANUAL section 6 on the map bug), and the
;;; cheap discriminator here failed too: the outer fiber IS in
;;; active_fibers in this very case, so "the pumping fiber is unrooted" is
;;; not the whole story. Next step is a watchpoint on the closure's
;;; header, or marking with a sentinel sweep, not more reading.

(display (touch (future (touch (future 42)))))
(newline)
(display "survived")
(newline)
