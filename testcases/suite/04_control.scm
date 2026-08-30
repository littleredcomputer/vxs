;;----------------------------------------------------------------------
;; Layer 04: Control Flow, Iteration, Lazy Streams & Higher-Order
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")

(test-suite "04_control: Conditionals, Loops, Promises & Higher-Order")

;; 1. Branching & Conditionals (if, cond, case)
(assert-equal "if 3-arg true" 1 (if #t 1 2))
(assert-equal "if 3-arg false" 2 (if #f 1 2))
(assert-equal "if 2-arg true" 42 (if #t 42))
(assert-equal "if 2-arg false returns unspecified" (void) (if #f 42))

(define (classify-num n)
  (cond
    ((< n 0) 'negative)
    ((= n 0) 'zero)
    ((< n 10) 'small)
    (else 'large)))

(assert-equal "cond branch 1" 'negative (classify-num -5))
(assert-equal "cond branch 2" 'zero (classify-num 0))
(assert-equal "cond branch 3" 'small (classify-num 7))
(assert-equal "cond else branch" 'large (classify-num 999))

(define (describe-val x)
  (case x
    ((1 2 3) 'small-int)
    ((#\a #\b) 'char-ab)
    ((foo bar) 'symbol-foobar)
    (else 'other)))

(assert-equal "case match 1" 'small-int (describe-val 2))
(assert-equal "case match 2" 'char-ab (describe-val #\b))
(assert-equal "case match 3" 'symbol-foobar (describe-val 'foo))
(assert-equal "case else match" 'other (describe-val "unmatched"))

;; 2. Sequencing (begin, when, unless)
(let ((x 0))
  (begin
    (set! x (+ x 10))
    (set! x (+ x 20))
    (set! x (+ x 5)))
  (assert-equal "begin side effects" 35 x))

(let ((w 0))
  (when #t (set! w 100))
  (assert-equal "when executed" 100 w)
  (when #f (set! w 200))
  (assert-equal "when not executed" 100 w))

(let ((u 0))
  (unless #f (set! u 50))
  (assert-equal "unless executed" 50 u)
  (unless #t (set! u 100))
  (assert-equal "unless not executed" 50 u))

;; 3. Iteration (do loops)
;; Basic single-variable do
(define do-res-1
  (do ((i 0 (+ i 1)))
      ((= i 5) i)))
(assert-equal "do single-variable loop" 5 do-res-1)

;; Multi-variable do with step and accumulator
(define do-res-2
  (do ((i 1 (+ i 1))
       (sum 0 (+ sum i)))
      ((> i 10) sum)))
(assert-equal "do multi-variable sum 1..10" 55 do-res-2)

;; Bracketed do loop
(define do-res-3
  (do [i 1 (+ i 1) prod 1 (* prod i)]
      [(> i 5) prod]))
(assert-equal "bracketed do loop factorial 5" 120 do-res-3)

;; Do loop with commands and internal mutation
(define do-res-4
  (let ((log '()))
    (do ((i 0 (+ i 1)))
        ((= i 3) log)
      (set! log (cons i log)))))
(assert-equal "do with body commands mutation" '(2 1 0) do-res-4)

;; 4. Lazy Evaluation (delay & force)
(let* ((evaluated? #f)
       (p (delay (begin (set! evaluated? #t) 42))))
  (assert-false "promise not evaluated initially" evaluated?)
  (assert-equal "first force evaluates promise" 42 (force p))
  (assert-true "promise was evaluated" evaluated?)
  (assert-equal "second force returns result" 42 (force p)))

;; Infinite lazy stream
;; Must be a macro, not a procedure: a procedure's arguments are evaluated
;; eagerly at the call site, so (stream-cons n (integers-from (+ n 1)))
;; would recurse infinitely before stream-cons ever got a chance to wrap
;; the second argument in delay. Expanding to (cons a (delay b)) leaves b
;; as the raw, unevaluated expression, so delay is what defers it.
(defmacro stream-cons (a b) `(cons ,a (delay ,b)))
(define (stream-car s) (car s))
(define (stream-cdr s) (force (cdr s)))

(define (integers-from n)
  (stream-cons n (integers-from (+ n 1))))

(define (stream-take s n)
  (if (<= n 0)
      '()
      (cons (stream-car s) (stream-take (stream-cdr s) (- n 1)))))

(assert-equal "lazy infinite stream take 5" '(1 2 3 4 5) (stream-take (integers-from 1) 5))

;; 5. Higher-Order Procedures (apply, map, for-each, filter)
(assert-equal "apply n-ary leading args" 15 (apply + 1 2 3 '(4 5)))
(assert-equal "apply with single list" 24 (apply * '(2 3 4)))
(assert-equal "map double" '(2 4 6 8) (map (lambda (x) (* x 2)) '(1 2 3 4)))

(let ((acc 0))
  (for-each (lambda (x) (set! acc (+ acc x))) '(10 20 30))
  (assert-equal "for-each accumulation" 60 acc))

(assert-equal "filter even numbers" '(2 4 6 8) (filter even? '(1 2 3 4 5 6 7 8)))
(assert-equal "filter positive numbers" '(10 20) (filter positive? '(-5 10 -2 20 0)))


;;--- case rejects a malformed clause ------------------------------------
;; R7RS: a clause is ((datum ...) expression ...) or (else expression ...).
;; All three departures used to compile, run, and silently do nothing —
;; and two of them are the same typo, a paren after the DATUM LIST rather
;; than after the body:
;;
;;   ((:finish))  (begin ...)     the begin becomes a stray clause, and the
;;                                clause that matched has an empty body, so
;;                                the procedure returns unspecified
;;
;; That cost two real bugs in one afternoon before this check existed. It
;; is invisible by construction: the form is still well-formed Scheme, it
;; still compiles, it still runs, and it returns a value.
;;
;; Checked by compiling rather than running, so a fiber boundary catches
;; what the compiler raises.
;; The three rejections are NOT asserted here: the check fires while the
;; enclosing form is COMPILED, so provoking it needs `eval`, which vxs does
;; not have (R7RS's separate (scheme eval) library). Verified by hand:
;;
;;   (case 1 ((1)) (else 2))
;;   case: clause for (1) has no body — a stray paren after the datum list
;;   is the usual cause
;;
;;   (case 9 ((1) 2) (display "x") (else 3))
;;   case: a clause's datums must be a LIST, got display
;;
;;   (case 1 (1 2) (else 3))
;;   case: a clause's datums must be a LIST, got 1 — write ((1) body ...)

;; Valid forms first, so a blanket rejection would fail here.
(assert-equal "an ordinary case still works" 2 (case 1 ((1) 2) (else 3)))
(assert-equal "with several datums per clause"
              'b (case 5 ((1 2) 'a) ((5 6) 'b) (else 'c)))
(assert-equal "and a multi-expression body"
              3 (case 1 ((1) 1 2 3) (else 0)))
(assert-equal "else still catches"
              'fell (case 99 ((1) 'no) (else 'fell)))
(assert-equal "an empty datum list never matches"
              'fell (case 1 (() 'no) (else 'fell)))

(suite-summary)
