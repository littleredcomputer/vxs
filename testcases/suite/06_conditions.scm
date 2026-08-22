;;----------------------------------------------------------------------
;; Layer 06: unwind-protect, guard/raise, multiple values, vector/string
;; map family, small numeric additions
;;
;; Everything in this file was built and verified in one session via
;; ad-hoc /tmp scripts that no longer exist — this is that verification
;; made permanent. Depth matters more than breadth here: each feature's
;; hard cases (yield mid-body, reentrant/nested unwinding, parallel vs.
;; sequential binding, GC pressure during unwinding) are the actual
;; regressions worth catching, not just "does the happy path work."
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")

(test-suite "06_conditions: unwind-protect, guard/raise, values, vector/string maps")

;; A small local helper — not in test_framework.scm since it's only
;; useful where "does this raise at all" is the question, which is
;; mostly just this file.
(define (raises? thunk)
  (guard (e (#t #t)) (thunk) #f))

;; 1. unwind-protect — normal exit
(assert-equal "unwind-protect normal exit returns body's value"
              3 (unwind-protect (+ 1 2) 'ignored))

(let ((cleanup-ran? #f))
  (unwind-protect (+ 1 2) (set! cleanup-ran? #t))
  (assert-true "unwind-protect cleanup runs on normal exit" cleanup-ran?))

;; Multiple cleanup forms, closure capture of an outer variable
(let ((counter 0))
  (unwind-protect
    'body
    (set! counter (+ counter 1))
    (set! counter (+ counter 1)))
  (assert-equal "unwind-protect runs all cleanup forms, closing over outer state"
                2 counter))

;; 2. unwind-protect — escape via call/cc
(let ((log '()))
  (call-with-current-continuation
    (lambda (k)
      (unwind-protect
        (begin (set! log (cons 'body-ran log)) (k 'escaped) (set! log (cons 'unreachable log)))
        (set! log (cons 'cleanup-ran log)))))
  (assert-equal "unwind-protect cleanup runs when body escapes via call/cc, and only that far"
                '(cleanup-ran body-ran) log))

;; 3. unwind-protect — nested, innermost cleanup runs first on escape
(let ((log '()))
  (call-with-current-continuation
    (lambda (k)
      (unwind-protect
        (unwind-protect
          (begin (set! log (cons 'inner-body log)) (k 'out))
          (set! log (cons 'inner-cleanup log)))
        (set! log (cons 'outer-cleanup log)))))
  (assert-equal "nested unwind-protect cleans up innermost-first on escape"
                '(outer-cleanup inner-cleanup inner-body) log))

;; 4. unwind-protect — cleanup survives (yield), the reason it compiles
;; inline instead of as a subr
(let ((log '()))
  (future
    (unwind-protect
      (begin
        (set! log (cons 'before-yield log))
        (yield)
        (set! log (cons 'after-yield log)))
      (set! log (cons 'cleanup log))))
  (run-fibers)
  (assert-equal "unwind-protect's pending cleanup survives fiber suspension"
                '(cleanup after-yield before-yield) log))

;; 5. unwind-protect — cleanup runs when the body raises (not a native
;; VM error like (car '()) — see the note at the bottom of this file:
;; guard can only catch things raised via raise/error today, not the
;; native "contract violation" errors car/cdr/vector-ref/etc. still
;; signal directly against Fiber::State::Error).
(let ((cleanup-ran? #f))
  (guard (e (#t 'caught))
    (unwind-protect
      (error "trigger")
      (set! cleanup-ran? #t)))
  (assert-true "unwind-protect cleanup runs even when the body raises" cleanup-ran?))

;; 6. guard/raise — basic catch of an arbitrary raised value
(assert-equal "guard catches a raised symbol"
              '(caught boom) (guard (e (#t (list 'caught e))) (raise 'boom)))

;; 7. guard/raise — non-raising body just returns its value
(assert-equal "guard is transparent when the body doesn't raise"
              3 (guard (e (#t 'never)) (+ 1 2)))

;; 8. error / error-object accessors, caught by guard
(assert-equal "error is catchable and carries message + irritants"
              '("broke" (1 2 3))
              (guard (e ((error-object? e)
                         (list (error-object-message e) (error-object-irritants e))))
                (error "broke" 1 2 3)))

;; 9. guard — unmatched clause auto-re-raises to an outer guard
(assert-equal "an unmatched guard clause re-raises to the next enclosing guard"
              '(outer-caught 42)
              (guard (outer (#t (list 'outer-caught outer)))
                (guard (inner ((symbol? inner) 'never))   ; 42 isn't a symbol
                  (raise 42))))

;; 10. guard — explicit else clause doesn't get a duplicate auto-else
(assert-equal "guard with an explicit else clause works (no double-else)"
              '(else-branch x) (guard (e (#f 'nope) (else (list 'else-branch e))) (raise 'x)))

;; 11. guard's handler is a genuine cond — inherits => for free
(assert-equal "guard clauses support cond's => for free"
              'two
              (guard (e ((assv e '((1 . one) (2 . two))) => cdr) (else 'no-match))
                (raise 2)))

;; 12. guard composes with unwind-protect: cleanup runs before the
;; handler sees the value, not after
(let ((log '()))
  (guard (e (#t (set! log (cons (list 'guard-saw e) log))))
    (unwind-protect
      (begin (set! log (cons 'body-ran log)) (raise 'oops))
      (set! log (cons 'cleanup-ran log))))
  (assert-equal "unwind-protect cleanup runs before guard's handler sees the raised value"
                '((guard-saw oops) cleanup-ran body-ran) log))

;; 13. values / call-with-values — single value is never wrapped
(assert-equal "call-with-values with an ordinary (non-values) producer"
              84 (call-with-values (lambda () 42) (lambda (x) (* x 2))))
(assert-equal "(values x) with one argument passes through unwrapped"
              84 (call-with-values (lambda () (values 42)) (lambda (x) (* x 2))))

;; 14. values with multiple / zero values
(assert-equal "call-with-values with multiple values"
              6 (call-with-values (lambda () (values 1 2 3)) +))
(assert-equal "call-with-values with zero values"
              'zero-ok (call-with-values (lambda () (values)) (lambda () 'zero-ok)))

;; 15. values with a dotted/variadic consumer formal list
(assert-equal "call-with-values consumer can be variadic"
              '(1 2 3 4) (call-with-values (lambda () (values 1 2 3 4)) list))

;; 16. let-values / let*-values basic
(assert-equal "let-values destructures multiple values"
              '(1 2 3) (let-values (((a b) (values 1 2)) ((c) (values 3))) (list a b c)))
(assert-equal "let*-values sees earlier bindings"
              '(1 2 3) (let*-values (((a b) (values 1 2)) ((c) (+ a b))) (list a b c)))

;; 17. let-values is genuinely PARALLEL — the distinguishing case. An
;; inner clause's producer expression must see the OUTER `a`, not the
;; one being bound by an earlier clause in the SAME let-values — this
;; is exactly what let*-values would get wrong (or rather, right for a
;; different reason) if the two forms were accidentally compiled the
;; same way.
(define a 100)
(assert-equal "let-values clauses are parallel: later producers don't see earlier bindings"
              '(1 100) (let-values (((a) (values 1)) ((b) (values a))) (list a b)))

;; 18. vector-map / vector-for-each
(assert-equal "vector-map single vector" [1 4 9 16] (vector-map (lambda (x) (* x x)) #(1 2 3 4)))
(assert-equal "vector-map N-ary walks to the shortest vector"
              [11 22 33] (vector-map + #(1 2 3 4 5) #(10 20 30)))
(let ((acc '()))
  (vector-for-each (lambda (x) (set! acc (cons x acc))) #(a b c))
  (assert-equal "vector-for-each visits in order" '(c b a) acc))
;; vector-for-each/string-map/for-each's own "wrong type" errors (below)
;; are NOT exercised here with raises? — they signal directly against
;; Fiber::State::Error rather than raising (see the note at the bottom
;; of this file), so guard can't catch them and a real one would abort
;; this whole suite process the same way (car '()) did during this
;; file's own first draft. Verified manually instead; not yet
;; expressible as an in-process regression test.

;; 19. string-map / string-for-each
(assert-equal "string-map" "HELLO" (string-map char-upcase "hello"))
(let ((acc '()))
  (string-for-each (lambda (c) (set! acc (cons c acc))) "abc")
  (assert-equal "string-for-each visits in order" '(#\c #\b #\a) acc))

;; 20. for-each still refuses non-lists (the fix that started this —
;; silently doing nothing was the bug; raising cleanly is the contract)
;; — again native-error-shaped, not guard-catchable; see note above.
;; What IS safely testable in-process: the non-error paths.
(assert-equal "for-each on '() is a normal no-op, not an error"
              #f (raises? (lambda () (for-each display '()))))
(let ((seen '()))
  (for-each (lambda (a b) (set! seen (cons (cons a b) seen))) '(1 2 3) '(a b c d e))
  (assert-equal "for-each with differing-length proper lists stops at the shortest, no error"
                '((3 . c) (2 . b) (1 . a)) seen))

;; 21. Small numeric additions
(assert-true "exact-integer? true for an exact int" (exact-integer? 5))
(assert-false "exact-integer? false for an inexact whole number" (exact-integer? 5.0))
(assert-true "nan? detects NaN" (nan? (/ 0.0 0.0)))
(assert-false "nan? false for an ordinary number" (nan? 5))
(assert-true "and NaN from a domain error" (nan? (sqrt -1.0)))
(assert-false "a NaN is not equal to itself, per IEEE"
              (= (/ 0.0 0.0) (/ 0.0 0.0)))
(assert-false "infinities are not NaN and must survive untouched"
              (or (nan? (/ 1.0 0.0)) (nan? (/ -1.0 0.0))))

;;--- EVERY NaN must canonicalize ----------------------------------------
;; The tag space 0xFFF8-0xFFFF IS the negative-quiet-NaN range, so a NaN
;; that reaches storage unchanged does not merely misprint — it becomes
;; another type:
;;
;;   0xFFF8  the empty list     0xFFFE  an integer
;;   0xFFF9  #f                 0xFFFF  a POINTER, dereferenced
;;
;; from_double used to test the bits by hand and required the quiet bit to
;; be CLEAR, so it caught only signalling NaNs. On arm64 0.0/0.0 yields a
;; POSITIVE quiet NaN and nothing showed; on x86-64 it yields
;; 0xFFF8000000000000 and (nan? (/ 0.0 0.0)) was #f, because the NaN had
;; become the empty list. The last two cases below used to SIGSEGV on the
;; read, on every architecture, and gpu-buffer-read hands back device bytes
;; that a program may legitimately view as :f64.

;; Spelled through string->number because the reader has no #x literals
;; (see MANUAL.md §6) — which is just as well here, since the hex is the
;; whole point and a decimal constant would hide it.
(define (f64-from-hi hex)
  (let* ((b (make-bytes 8))
         (u (bytes-view b :u32))
         (f (bytes-view b :f64)))
    (view-set! u 0 0)
    (view-set! u 1 (string->number hex 16))
    (view-ref f 0)))

(assert-true "a positive quiet NaN reads as NaN"     (nan? (f64-from-hi "7FF80000")))
(assert-true "a NEGATIVE quiet NaN is not the empty list" (nan? (f64-from-hi "FFF80000")))
(assert-false "and really is not"                    (null? (f64-from-hi "FFF80000")))
(assert-true "nor #f"                                (nan? (f64-from-hi "FFF90000")))
(assert-true "nor an integer"                        (nan? (f64-from-hi "FFFE0000")))
(assert-false "and really is not that either"        (integer? (f64-from-hi "FFFE0000")))
;; This one dereferenced the payload as a pointer.
(assert-true "nor a pointer"                         (nan? (f64-from-hi "FFFF0000")))
(assert-true "with every payload bit set either"     (nan? (f64-from-hi "FFFFFFFF")))
(assert-true "infinite? detects infinity" (infinite? (/ 1.0 0.0)))
(assert-false "infinite? false for a finite float" (infinite? 5.0))
(assert-true "finite? true for an exact integer" (finite? 5))
(assert-false "finite? false for infinity" (finite? (/ 1.0 0.0)))
(assert-equal "square of an exact int" 49 (square 7))
(assert-equal "square of a float" 12.25 (square 3.5))

;; 22. assert — captures the failing EXPRESSION, not just its value
(assert-equal "assert returns unspecified on success" (void) (assert (= 1 1)))
(assert-true "assert raises on failure" (raises? (lambda () (assert (= 1 2)))))
(assert-true "assert's error mentions the failing expression, not just #f"
             (guard (e (#t (let ((irritants (error-object-irritants e)))
                             (and (pair? irritants) (member '(= 1 2) irritants)))))
               (assert (= 1 2))
               #f))

;; 23. number->string / string->number exactness round-trip — the bug
;; that cost 10 of r4rstest.scm's failures: number->string used to
;; print whole-number doubles as bare integer text ("0" for 0.0), so
;; string->number read them back as exact, and (eqv? 0.0 0) is false.
(assert-equal "number->string preserves a whole-number float's decimal point"
              "0.0" (number->string 0.0))
(assert-equal "number->string round-trip preserves exactness for whole-number floats"
              #t (eqv? 42.0 (string->number (number->string 42.0))))
(assert-equal "number->string of an exact integer has no decimal point"
              "42" (number->string 42))

;; 24. Reentrant delay/force — a promise whose own body forces itself
;; once more before completing. The naive memoization algorithm gets
;; this wrong (the outer call's own result clobbers the inner call's
;; already-memoized one); R4RS's reference algorithm doesn't.
(assert-equal "reentrant force: inner completion wins over the outer call's own result"
              3 (letrec ((p (delay (if c 3 (begin (set! c #t) (+ (force p) 1)))))
                         (c #f))
                  (force p)))
;; Ordinary (non-reentrant) memoization still only evaluates once
(let ((count 0))
  (define p (delay (begin (set! count (+ count 1)) count)))
  (force p) (force p)
  (assert-equal "delay still memoizes normally (single evaluation)" 1 count))

;; NOTE on what's NOT covered above: guard only catches things raised
;; via raise/error. The large family of native "contract violation"
;; errors — car/cdr on a non-pair, wrong arity, unbound variables, and
;; the type checks vector-for-each/string-map/string-for-each/for-each
;; themselves added — still signal by setting Fiber::State::Error
;; directly (a mechanism that predates raise/guard entirely) rather
;; than throwing. guard's C++ try/catch never sees them, so a real one
;; is uncatchable and fatal to the whole process, in-suite or not —
;; confirmed by hand: (guard (e (#t 'caught)) (car '())) still aborts.
;; Unifying that with raise (so ALL runtime errors are guard-catchable,
;; as R7RS actually specifies) is real, valuable, out-of-scope-for-
;; today follow-up work, not a gap in this file's coverage.

(suite-summary)
