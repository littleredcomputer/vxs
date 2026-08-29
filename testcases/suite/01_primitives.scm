;;----------------------------------------------------------------------
;; Layer 01: Primitives & Core Data Structures
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")

(test-suite "01_primitives: Core Data & Operations")

;; 1. Numbers & Basic Arithmetic
(assert-equal "addition 0-ary" 0 (+))
(assert-equal "addition 1-ary" 42 (+ 42))
(assert-equal "addition N-ary" 15 (+ 1 2 3 4 5))
(assert-equal "subtraction 1-ary (negation)" -10 (- 10))
(assert-equal "subtraction N-ary" 14 (- 20 2 4))
(assert-equal "multiplication 0-ary" 1 (*))
(assert-equal "multiplication N-ary" 120 (* 1 2 3 4 5))
(assert-equal "float division" 2.5 (/ 5 2))
(assert-equal "quotient" 3 (quotient 10 3))
(assert-equal "remainder" 1 (remainder 10 3))
(assert-equal "modulo" 1 (modulo 10 3))
(assert-equal "abs positive" 42 (abs 42))
(assert-equal "abs negative" 42 (abs -42))
(assert-equal "sqrt exact" 4.0 (sqrt 16))

;; 2. 64-Bit Integer & Overflow Promotion
(assert-equal "64-bit addition overflow to double" 4000000000.0 (+ 2000000000 2000000000))
(assert-equal "64-bit multiplication overflow to double" 1000000000000.0 (* 1000000 1000000))
(assert-equal "large quotient" 1000000.0 (quotient (* 1000000 1000000) 1000000))
(assert-equal "large remainder" 0 (remainder (* 1000000 1000000) 1000000))

;; 3. Numeric Predicates & Comparisons
(assert-true "zero? on 0" (zero? 0))
(assert-false "zero? on 1" (zero? 1))
(assert-true "positive? on 5" (positive? 5))
(assert-false "positive? on -5" (positive? -5))
(assert-true "negative? on -3" (negative? -3))
(assert-false "negative? on 3" (negative? 3))
(assert-true "even? on 8" (even? 8))
(assert-false "even? on 9" (even? 9))
(assert-true "odd? on 7" (odd? 7))
(assert-false "odd? on 6" (odd? 6))

(assert-true "numeric =" (= 10 10))
(assert-false "numeric = mismatch" (= 10 20))
(assert-true "numeric <" (< 5 10))
(assert-false "numeric < false" (< 10 5))
(assert-true "numeric <=" (<= 5 5))
(assert-true "numeric >" (> 10 5))
(assert-true "numeric >=" (>= 10 10))

;; 4. Equality (eq?, eqv?, equal?)
(assert-true "eq? on symbols" (eq? 'foo 'foo))
(assert-false "eq? on distinct symbols" (eq? 'foo 'bar))
(assert-true "eq? on booleans" (eq? #t #t))
(assert-true "eq? on empty list" (eq? '() '()))
(assert-true "equal? on deep lists" (equal? '(1 (2 3) 4) '(1 (2 3) 4)))
(assert-false "equal? on different lists" (equal? '(1 2 3) '(1 2 4)))
(assert-true "equal? on vectors" (equal? [1 2 3] [1 2 3]))
(assert-false "equal? on different vectors" (equal? [1 2 3] [1 2 4]))
(assert-true "equal? on strings" (equal? "hello" "hello"))
(assert-false "equal? on different strings" (equal? "hello" "world"))

;; 5. Booleans & Logic
(assert-true "boolean? on #t" (boolean? #t))
(assert-true "boolean? on #f" (boolean? #f))
(assert-false "boolean? on 0" (boolean? 0))
(assert-true "not on #f" (not #f))
(assert-false "not on #t" (not #t))
(assert-false "not on truthy number" (not 0))
(assert-true "and all true" (and #t 1 'foo))
(assert-false "and with false" (and #t #f 1))
(assert-equal "or first truthy" 42 (or #f #f 42 #f))
(assert-false "or all false" (or #f #f #f))

;; 6. Pairs & Lists
(assert-true "pair? on cons" (pair? (cons 1 2)))
(assert-false "pair? on nil" (pair? '()))
(assert-true "null? on nil" (null? '()))
(assert-false "null? on pair" (null? (cons 1 2)))
(assert-equal "car and cdr" '(1 . 2) (cons (car '(1 . 2)) (cdr '(1 . 2))))
(assert-equal "cadr" 2 (cadr '(1 2 3)))
(assert-equal "caddr" 3 (caddr '(1 2 3)))
(assert-equal "caar" 1 (caar '((1 2) 3)))

;; A contract violation is a native VM error and guard cannot catch it
;; (MANUAL section 3), so the tests below observe one across a FIBER
;; boundary instead — the documented way, and the same route a supervisor
;; would use.
(define (fails? thunk)
  (let ((f (future (thunk))))
    (run-fibers)
    (error-object? (touch/or-error f))))

;; string-append used to skip anything that was neither a string nor a
;; symbol, so (string-append "a" 5) was "a": an argument discarded and a
;; plausible result returned. That is worse than a wrong value — there is
;; nothing left to notice.
(assert-equal "string-append refuses a number"
              #t (fails? (lambda () (string-append "a" 5))))
(assert-equal "and a list"
              #t (fails? (lambda () (string-append "a" (list 1 2)))))
(assert-equal "and a boolean"
              #t (fails? (lambda () (string-append "a" #t))))

;; substring had the same shape of leniency, in five places at once.
;; R7RS requires 0 <= start <= end <= length and calls a violation an
;; error; each one used to be papered over into a plausible result.
(assert-equal "substring refuses a non-string"
              #t (fails? (lambda () (substring 5 0 1))))
(assert-equal "an end past the end is not clamped"
              #t (fails? (lambda () (substring "abc" 1 99))))
(assert-equal "start after end is not silently empty"
              #t (fails? (lambda () (substring "abc" 2 1))))
(assert-equal "a negative start is not clamped to zero"
              #t (fails? (lambda () (substring "abc" -3 2))))
;; The one that decided it: as_int on a string reinterprets whatever bits
;; the value holds, which for a heap object is a pointer. It happened to
;; give 0.
(assert-equal "and a string where an index belongs"
              #t (fails? (lambda () (substring "abc" "x" 2))))
(assert-equal "nor a fractional index"
              #t (fails? (lambda () (substring "abc" 0 1.5))))

;; Indices are any INTEGRAL number rather than strictly exact integers,
;; because (/ 4 2) is inexact here and (substring s 0 (/ n 2)) is a
;; reasonable thing to write.
(assert-equal "an inexact but whole index is accepted"
              "ab" (substring "abcd" 0 (/ 4 2)))
(assert-equal "ordinary substrings are unaffected"
              '("bcd" "abc" "") (list (substring "abcdef" 1 4)
                                      (substring "abc" 0 3)
                                      (substring "abc" 2 2)))

;; Symbols ARE still coerced. A deliberate extension rather than an
;; oversight — R7RS takes strings only — kept because it is lossless and
;; this codebase builds messages out of symbol names.
(assert-equal "but a symbol is coerced, deliberately"
              "ab" (string-append "a" 'b))
(assert-equal "and ordinary appends are unaffected"
              "abc" (string-append "a" "b" "c"))
(assert-equal "including the zero-argument case" "" (string-append))

;; The compound accessors must FAIL on a short list, not return nil.
;; R7RS defines (cadr x) as (car (cdr x)), so (cadr '(1)) is (car '()) and
;; must raise — and car itself does, so returning nil made the two
;; disagree. A short list silently yielded () and the mistake surfaced
;; somewhere else entirely, which is the worst way to find a destructuring
;; error.
;;
;; Checked across a FIBER boundary — see the note on fails? above.

(assert-equal "cadr of a one-element list fails"   #t (fails? (lambda () (cadr '(1)))))
(assert-equal "caddr of a two-element list fails"  #t (fails? (lambda () (caddr '(1 2)))))
(assert-equal "cadddr of a three-element list fails" #t (fails? (lambda () (cadddr '(1 2 3)))))
(assert-equal "caar of a flat list fails"          #t (fails? (lambda () (caar '(1 2)))))
(assert-equal "cddr of a one-element list fails"   #t (fails? (lambda () (cddr '(1)))))
(assert-equal "and on a non-list entirely"         #t (fails? (lambda () (cadr 5))))

;; The valid cases still work, so the fix did not simply make them strict
;; about the wrong thing.
(assert-equal "cddr still returns the tail" '(3) (cddr '(1 2 3)))
(assert-equal "cdar still works"            '(2) (cdar '((1 2) 3)))
(assert-equal "cadddr still works"          4    (cadddr '(1 2 3 4)))
(assert-equal "cdar" '(2) (cdar '((1 2) 3)))
(assert-equal "list length" 4 (length '(a b c d)))
(assert-equal "list reverse" '(4 3 2 1) (reverse '(1 2 3 4)))
(assert-equal "list append" '(1 2 3 4 5 6) (append '(1 2) '(3 4) '(5 6)))
(assert-equal "memq found" '(b c) (memq 'b '(a b c)))
(assert-false "memq not found" (memq 'z '(a b c)))
(assert-equal "assq found" '(b 20) (assq 'b '((a 10) (b 20) (c 30))))
(assert-false "assq not found" (assq 'z '((a 10) (b 20))))

;; Mutating pairs
(let ((p (cons 10 20)))
  (set-car! p 99)
  (set-cdr! p 100)
  (assert-equal "set-car! & set-cdr!" '(99 . 100) p))

;; 7. Vectors
(assert-true "vector? on vector" (vector? [1 2 3]))
(assert-false "vector? on list" (vector? '(1 2 3)))
(assert-equal "vector constructor" 3 (vector-length (vector 10 20 30)))
(assert-equal "make-vector initialized" [7 7 7 7] (make-vector 4 7))
(assert-equal "vector-ref" 20 (vector-ref [10 20 30] 1))
(let ((v (vector 1 2 3)))
  (vector-set! v 1 999)
  (assert-equal "vector-set!" [1 999 3] v))
(assert-equal "vector as procedure call" 200 ([100 200 300] 1))

;; 8. Strings & Characters
(assert-equal "string-length" 5 (string-length "hello"))
(assert-equal "string-ref" #\e (string-ref "hello" 1))
(assert-equal "string-append" "hello world" (string-append "hello" " " "world"))
(assert-equal "substring" "ell" (substring "hello" 1 4))
(assert-equal "number->string int" "12345" (number->string 12345))
(assert-equal "number->string float" "3.14" (number->string 3.14))
(assert-equal "string->number int" 12345 (string->number "12345"))
(assert-equal "string->number float" 3.14 (string->number "3.14"))
(assert-true "char? on char" (char? #\A))
(assert-false "char? on string" (char? "A"))
(assert-true "char=?" (char=? #\A #\A))
(assert-false "char=? mismatch" (char=? #\A #\B))
(assert-equal "char->integer" 65 (char->integer #\A))
(assert-equal "integer->char" #\A (integer->char 65))

;; 9. Keywords & Associative Maps
(assert-true "keyword? on :foo" (keyword? :foo))
(assert-false "keyword? on symbol" (keyword? 'foo))
(assert-equal "keyword->string" "foo" (keyword->string :foo))
(assert-equal "string->keyword" :bar (string->keyword "bar"))

(let ((m {:name "vxs" :version 0.8 :fast? #t}))
  (assert-true "map? on map" (map? m))
  (assert-equal "get from map" "vxs" (get m :name))
  (assert-equal "map as procedure" 0.8 (m :version))
  (assert-equal "keyword as procedure" #t (:fast? m)))

(suite-summary)
