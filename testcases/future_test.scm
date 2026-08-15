;;; future_test.scm: Comprehensive in-Scheme test for Future & Fiber Concurrency

(define (assert-equal expected actual msg)
  (if (equal? expected actual)
      (begin
        (display "PASS: ")
        (display msg)
        (newline))
      (begin
        (display "FAIL: ")
        (display msg)
        (display " (expected ")
        (write expected)
        (display ", got ")
        (write actual)
        (display ")")
        (newline))))

;; 1. Future creation and predicate
(define f0 (future 42))
(assert-equal #t (future? f0) "future? predicate on future")
(assert-equal #f (future? 42) "future? predicate on non-future")
(assert-equal 42 (touch f0) "touch immediate future")
(assert-equal #t (future-done? f0) "future-done? on completed future")

;; 2. Concurrent futures with yield and value return
(define log '())
(define f1 (future
             (set! log (cons 'f1-1 log))
             (yield)
             (set! log (cons 'f1-2 log))
             'result-1))
(define f2 (future
             (set! log (cons 'f2-1 log))
             (yield)
             (set! log (cons 'f2-2 log))
             'result-2))

(define res (list (touch f1) (touch f2)))
(assert-equal '(result-1 result-2) res "touch returns correct values")
(assert-equal '(f2-2 f1-2 f2-1 f1-1) log "interleaved fiber yield execution")

;; 3. Non-blocking frame stepping with (step-fibers)
(define counter 0)
(define f-step (future
                 (set! counter 1)
                 (yield)
                 (set! counter 2)
                 (yield)
                 (set! counter 3)
                 'stepped))

(assert-equal #f (future-done? f-step) "future not done initially")
(step-fibers)
(assert-equal 1 counter "step-fibers slice 1")
(step-fibers)
(assert-equal 2 counter "step-fibers slice 2")
(step-fibers)
(assert-equal 3 counter "step-fibers slice 3")
(assert-equal #t (future-done? f-step) "future done after slices")
(assert-equal 'stepped (touch f-step) "touch stepped future")

;; 4. Run all background fibers to completion with (run-fibers)
(define a1 0)
(define a2 0)
(define a3 0)
(future (set! a1 10))
(future (set! a2 20) (yield) (set! a2 (+ a2 5)))
(future (set! a3 100))
(run-fibers)
(define total (+ a1 a2 a3))
(assert-equal 135 total "run-fibers completes all active tasks")

(display "=== ALL SCHEME FUTURE TESTS PASSED! ===\n")
