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
(load "testcases/suite/10_string_ports.scm")
(load "testcases/suite/11_bytes_views.scm")
(load "testcases/suite/12_u32.scm")
(load "testcases/suite/13_threefry.scm")
(load "testcases/suite/14_prelude.scm")
(load "testcases/suite/15_wgsl.scm")
(load "testcases/suite/16_shadertoy.scm")
(load "testcases/suite/17_points.scm")
(load "testcases/suite/18_wrangle.scm")

(if (total-summary)
    (display "\n>>> ALL SUITES COMPLETED WITH ZERO ERRORS.\n\n")
    (begin
      (display "\n>>> TEST SUITE FAILED!\n\n")
      (exit 1)))
