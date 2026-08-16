;;----------------------------------------------------------------------
;; Layer 05: Garbage Collection Invariants & Stress Testing
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")

(test-suite "05_gc_stress: Memory Management & GC Invariants")

;; 1. Explicit GC in Idle State
(assert-equal "explicit gc call" (void) (gc))

;; 2. Local Variable Retention Across GC
(let ((a '(1 2 3 4 5))
      (b "hello gc world")
      (c [10 20 30 40 50])
      (d {:key "val"}))
  (gc)
  (assert-equal "list retained after GC" '(1 2 3 4 5) a)
  (assert-equal "string retained after GC" "hello gc world" b)
  (assert-equal "vector retained after GC" [10 20 30 40 50] c)
  (assert-equal "map retained after GC" "val" (get d :key)))

;; 3. Deep Allocation Stress & Transient Reclamation
(define (allocate-garbage n)
  (let loop ((i n) (acc 0))
    (if (<= i 0)
        acc
        (begin
          (if (= (remainder i 5000) 0) (gc))
          ;; Allocate short-lived transient structures
          (let ((garbage (cons i (cons (number->string i) [i (+ i 1)]))))
            (loop (- i 1) (+ acc 1)))))))

(assert-equal "50,000 transient allocations with GC" 50000 (allocate-garbage 50000))

;; 4. Cyclic Structure Reclamation (Mark-and-Sweep Termination)
(let ((cyclic-test
       (lambda ()
         (let ((node1 (list 1))
               (node2 (list 2)))
           (set-cdr! node1 node2)
           (set-cdr! node2 node1) ; create cycle: node1 -> node2 -> node1
           'cycle-created))))
  (cyclic-test)
  (gc)
  (assert-true "GC survived cyclic graph without infinite loop" #t))

;; 5. Upvalue Box Retention & Mutation Across Repeated GCs
(define (make-gc-counter init)
  (let ((val init))
    (lambda (step)
      (gc)
      (set! val (+ val step))
      val)))

(let ((counter (make-gc-counter 1000)))
  (assert-equal "upvalue step 1 + GC" 1010 (counter 10))
  (assert-equal "upvalue step 2 + GC" 1030 (counter 20))
  (assert-equal "upvalue step 3 + GC" 1060 (counter 30))
  (assert-equal "upvalue step 4 + GC" 1100 (counter 40)))

(suite-summary)
