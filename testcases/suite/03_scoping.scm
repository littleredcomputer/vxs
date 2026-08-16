;;----------------------------------------------------------------------
;; Layer 03: Scoping, Closures, Upvalues & Tail Recursion
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")

(test-suite "03_scoping: Lexical Closures, Shared Upvalues & TCO")

;; 1. Lexical Scoping & Multi-Level Closures
(define (make-adder x)
  (lambda (y) (+ x y)))

(define add10 (make-adder 10))
(assert-equal "1-level closure" 42 (add10 32))

(define (curry3 a)
  (lambda (b)
    (lambda (c)
      (+ a (* b c)))))

(assert-equal "3-level nested closure" 23 (((curry3 5) 6) 3))

;; 2. Shared Mutable Upvalue Cells (set! across scopes)
(define (make-counter init)
  (let ((count init))
    (list
     (lambda () count)                      ; get
     (lambda (n) (set! count (+ count n)))  ; inc!
     (lambda (n) (set! count (- count n)))) ; dec!
    ))

(let ((c (make-counter 100)))
  (define get-c (car c))
  (define inc-c (cadr c))
  (define dec-c (caddr c))

  (assert-equal "counter initial" 100 (get-c))
  (inc-c 25)
  (assert-equal "counter after inc!" 125 (get-c))
  (dec-c 50)
  (assert-equal "counter after dec!" 75 (get-c)))

;; Shared state between two closures created in same frame
(define (make-cell initial)
  (let ((v initial))
    (cons (lambda () v)
          (lambda (new-v) (set! v new-v)))))

(let* ((cell (make-cell "initial"))
       (getter (car cell))
       (setter (cdr cell)))
  (assert-equal "cell getter initial" "initial" (getter))
  (setter "updated")
  (assert-equal "cell getter updated" "updated" (getter)))

;; 3. Binding Forms: let, let*, letrec, named let
;; let (parallel binding semantics)
(let ((x 1) (y 2))
  (let ((x y) (y x))
    (assert-equal "let parallel binding x" 2 x)
    (assert-equal "let parallel binding y" 1 y)))

;; let* (sequential binding semantics)
(let* ((a 10)
       (b (+ a 5))
       (c (* a b)))
  (assert-equal "let* sequential bindings" 150 c))

;; letrec (mutually recursive local functions)
(letrec ((my-even? (lambda (n) (if (= n 0) #t (my-odd? (- n 1)))))
         (my-odd?  (lambda (n) (if (= n 0) #f (my-even? (- n 1))))))
  (assert-true "letrec mutual recursion even" (my-even? 20))
  (assert-true "letrec mutual recursion odd" (my-odd? 21)))

;; named let (looping)
(define fact-loop
  (lambda (n)
    (let loop ((i n) (acc 1))
      (if (<= i 1)
          acc
          (loop (- i 1) (* acc i))))))

(assert-equal "named let factorial" 3628800 (fact-loop 10))

;; Bracketed Binding Forms (Clojure-style syntax extensions)
(let [x 10 y 20]
  (assert-equal "bracketed let" 30 (+ x y)))

(let* [a 5 b (* a 2) c (+ a b)]
  (assert-equal "bracketed let*" 15 c))

(let loop [i 0 sum 0]
  (if (= i 10)
      (assert-equal "bracketed named let" 45 sum)
      (loop (+ i 1) (+ sum i))))

;; 4. Tail-Call Optimization (TCO)
;; 100,000-deep recursive loop must not overflow the stack or balloon memory
(define (countdown n)
  (if (<= n 0)
      'done
      (countdown (- n 1))))

(assert-equal "TCO 100,000-iteration tail recursion" 'done (countdown 100000))

;; Mutual tail recursion across 50,000 iterations
(define (tco-even? n)
  (if (<= n 0) #t (tco-odd? (- n 1))))

(define (tco-odd? n)
  (if (<= n 0) #f (tco-even? (- n 1))))

(assert-true "Mutual TCO 50,000 steps" (tco-even? 50000))

(suite-summary)
