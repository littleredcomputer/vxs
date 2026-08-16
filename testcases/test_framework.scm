;;----------------------------------------------------------------------
;; Vx-Scheme Ground-Up Test Framework
;;----------------------------------------------------------------------

(if (not (defined? '*test-framework-loaded*))
    (begin
      (define *test-framework-loaded* #t)
      (define *total-suites-run* 0)
      (define *total-tests-run* 0)
      (define *total-tests-passed* 0)
      (define *total-tests-failed* 0)))

(define *current-suite-name* "")
(define *current-suite-passed* 0)
(define *current-suite-failed* 0)

(define (test-suite name)
  (set! *current-suite-name* name)
  (set! *current-suite-passed* 0)
  (set! *current-suite-failed* 0)
  (set! *total-suites-run* (+ *total-suites-run* 1))
  (display "\n=== [TEST SUITE] ")
  (display name)
  (display " ===\n"))

(define (assert-equal name expected actual)
  (set! *total-tests-run* (+ *total-tests-run* 1))
  (if (equal? expected actual)
      (begin
        (set! *total-tests-passed* (+ *total-tests-passed* 1))
        (set! *current-suite-passed* (+ *current-suite-passed* 1))
        (display "  ✅ [PASS] ")
        (display name)
        (newline)
        #t)
      (begin
        (set! *total-tests-failed* (+ *total-tests-failed* 1))
        (set! *current-suite-failed* (+ *current-suite-failed* 1))
        (display "  ❌ [FAIL] ")
        (display name)
        (display "\n     Expected: ")
        (write expected)
        (display "\n     Actual:   ")
        (write actual)
        (newline)
        #f)))

(define (assert-true name actual)
  (assert-equal name #t (if actual #t #f)))

(define (assert-false name actual)
  (assert-equal name #f (if actual #t #f)))

(define (suite-summary)
  (display "─── Suite Result: ")
  (display *current-suite-passed*)
  (display " passed, ")
  (display *current-suite-failed*)
  (display " failed ───\n"))

(define (total-summary)
  (display "\n================================================================\n")
  (display "  TOTAL TEST SUMMARY\n")
  (display "  Suites Run:   ") (display *total-suites-run*) (newline)
  (display "  Tests Run:    ") (display *total-tests-run*) (newline)
  (display "  Tests Passed: ") (display *total-tests-passed*) (newline)
  (display "  Tests Failed: ") (display *total-tests-failed*) (newline)
  (display "================================================================\n")
  (if (= *total-tests-failed* 0)
      (begin
        (display "✨ ALL GROUND-UP TESTS PASSED SUCCESSFULLY! ✨\n")
        #t)
      (begin
        (display "❌ SOME TESTS FAILED!\n")
        #f)))
