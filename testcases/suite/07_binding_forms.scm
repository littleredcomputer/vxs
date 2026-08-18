;;----------------------------------------------------------------------
;; Layer 07: binding-form semantics under the inlined `let`
;;
;; `let` no longer compiles to ((lambda (v...) body) e...) — its bindings
;; live in local slots of the enclosing frame and its body is compiled
;; inline. That removed a closure allocation per scope entry, but it puts
;; weight on semantics that used to be guaranteed structurally by the
;; function call: fresh bindings per entry, parallel-vs-sequential
;; visibility, shadowing, and tail position.
;;
;; The load-bearing case is "fresh binding per entry" under capture.
;; Captured locals are boxed IN PLACE (OP_CLOSURE overwrites the stack
;; slot with an ObjUpvalue), so re-entering a scope must OVERWRITE the
;; slot rather than assign through the stale box — that is what
;; OP_INIT_LOCAL exists for, and why it must never be "simplified" into
;; OP_SET_LOCAL. If that ever regresses, the loop tests below return
;; (2 2 2) instead of (2 1 0) and nothing else in the suite notices.
;;
;; Verified ad hoc while the optimization was built; this is that
;; verification made permanent.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")

(test-suite "07_binding_forms: inlined let, fresh bindings, tail position")

;; --- THE invariant: each entry to a scope is a NEW binding -------------

(define captured-in-do '())
(do ((i 0 (+ i 1))) ((= i 3))
  (let ((x i))
    (set! captured-in-do (cons (lambda () x) captured-in-do))))
(assert-equal "closures over a let in a do-loop capture distinct bindings"
              '(2 1 0)
              (map (lambda (f) (f)) captured-in-do))

(define captured-in-named-let '())
(let loop ((i 0))
  (if (< i 3)
      (begin (let ((y (* 10 i)))
               (set! captured-in-named-let
                     (cons (lambda () y) captured-in-named-let)))
             (loop (+ i 1)))))
(assert-equal "same, driven by named-let recursion"
              '(20 10 0)
              (map (lambda (f) (f)) captured-in-named-let))

;; Two closures over the SAME entry must share one binding (they capture
;; the same box) — the mirror image of the test above.
(define shared
  (let ((n 0))
    (cons (lambda () (set! n (+ n 1)) n)
          (lambda () n))))
((car shared)) ((car shared))
(assert-equal "two closures over one entry share the binding" 2 ((cdr shared)))

;; --- captured bindings outlive their frame, independently --------------

(define (make-counter)
  (let ((n 0))
    (lambda () (set! n (+ n 1)) n)))
(define c1 (make-counter))
(define c2 (make-counter))
(c1) (c1) (c1)
(c2)
(assert-equal "set! through a captured binding (instance 1)" 4 (c1))
(assert-equal "captured bindings are per-instance (instance 2)" 2 (c2))

(define (adder k) (let ((base (* k 100))) (lambda (n) (+ base n))))
(assert-equal "escaping closures keep independent bindings"
              '(501 701)
              (list ((adder 5) 1) ((adder 7) 1)))

;; --- parallel vs sequential visibility ---------------------------------

(assert-equal "let initializers see OUTER bindings, not each other"
              '(1 2)
              (let ((a 1) (b 2))
                (let ((a b) (b a))
                  (list b a))))

(assert-equal "let* initializers see PREVIOUS bindings"
              6
              (let* ((a 1) (b (+ a 2)) (c (+ b 3))) c))

(assert-equal "letrec supports mutual recursion"
              #t
              (letrec ((ev? (lambda (n) (if (= n 0) #t (od? (- n 1)))))
                       (od? (lambda (n) (if (= n 0) #f (ev? (- n 1))))))
                (ev? 1000)))

;; --- shadowing and scope exit ------------------------------------------

(define shadow-me 'global)
(assert-equal "nested lets shadow innermost-first"
              'inner
              (let ((shadow-me 1))
                (let ((shadow-me 'middle))
                  (let ((shadow-me 'inner)) shadow-me))))
(assert-equal "outer binding intact after nested lets exit" 'global shadow-me)

;; Sibling scopes reuse slots; one must not see the other's values.
(assert-equal "sibling let scopes do not leak into each other"
              '(1 2)
              (list (let ((v 1)) v) (let ((v 2)) v)))

;; --- body shapes --------------------------------------------------------

(assert-equal "multi-form body returns its last value" 3 (let ((a 1)) 99 (+ a 2)))
(assert-equal "empty binding list" 7 (let () 7))
(assert-equal "let* with no bindings" 7 (let* () 7))

;; Internal defines in a let body take the letrec path, not the inline
;; one — they need their own scope rather than leaking outward.
(assert-equal "internal define inside a let body"
              12
              (let ((a 3))
                (define (quad n) (* 4 n))
                (quad a)))
(assert-equal "internal define does not leak to enclosing scope"
              #f
              (defined? 'quad))

;; --- tail position ------------------------------------------------------
;; A tail call from an inlined let body must remain a tail call; if the
;; body stopped being compiled in tail position this blows the stack.

(define (count-down n acc)
  (if (= n 0) acc (let ((m (- n 1))) (count-down m (+ acc 1)))))
(assert-equal "tail call from a let body is still a tail call"
              300000
              (count-down 300000 0))

(define (count-down* n acc)
  (if (= n 0) acc (let* ((m (- n 1)) (a (+ acc 1))) (count-down* m a))))
(assert-equal "tail call from a let* body is still a tail call"
              200000
              (count-down* 200000 0))

;; --- forms that desugar through let -------------------------------------

(assert-equal "do loop accumulates" 45
              (do ((i 0 (+ i 1)) (s 0 (+ s i))) ((= i 10) s)))
(assert-equal "do loop with empty body and result" 5
              (do ((i 0 (+ i 1))) ((= i 5) i)))
(assert-equal "case selects the right clause" 'medium
              (case 5 ((1 2 3) 'small) ((4 5 6) 'medium) (else 'large)))
(assert-equal "when/unless" '(yes no)
              (list (if #t 'yes 'no) (if #f 'yes 'no)))

(suite-summary)
