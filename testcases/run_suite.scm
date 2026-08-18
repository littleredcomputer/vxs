;;----------------------------------------------------------------------
;; vxs Master Test Suite Runner
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")

(display "\n================================================================\n")
(display "     🚀 RUNNING VXS GROUND-UP TEST SUITE 🚀\n")
(display "================================================================\n")

(load "testcases/suite/01_primitives.scm")
(load "testcases/suite/02_reader_macros.scm")
(load "testcases/suite/03_scoping.scm")
(load "testcases/suite/04_control.scm")
(load "testcases/suite/05_gc_stress.scm")
(load "testcases/suite/06_conditions.scm")
(load "testcases/suite/07_binding_forms.scm")
(load "testcases/suite/08_allocation.scm")
(load "testcases/suite/09_fibers.scm")

(if (total-summary)
    (display "\n>>> ALL SUITES COMPLETED WITH ZERO ERRORS.\n\n")
    (begin
      (display "\n>>> TEST SUITE FAILED!\n\n")
      (exit 1)))
