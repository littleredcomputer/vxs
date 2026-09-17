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

;;--- a subr's return value, in map --------------------------------------
;; `map` calls its function and conses the result onto the chain it is
;; building. A CLOSURE's result stays in a fiber stack slot that
;; mark_fiber reaches; a SUBR returns a bare Value that nothing refers to
;; but subr_map's own C++ local. The cons that stores it allocates, so the
;; collection it may trigger took the very value it was called to store.
;;
;; The result then had the right LENGTH and elements that were not pairs,
;; which is what makes this worth a test rather than a comment: nothing
;; crashed, and a caller that only checked the length saw nothing wrong.
;; It surfaced as `[stage "unbound index" th]` from a scan model's staged
;; log-joint, several layers away — see testcases/repro/scan_gc.scm.
;;
;; The input lists must be FRESHLY ALLOCATED. A quoted literal lives in
;; the constant pool and is permanently reachable, which hid this for as
;; long as the tests used one.

(define (gc-map-nary n)
  (let loop ((i 0) (s (list 1.0 2.0)) (bad 0))
    (if (= i n)
        bad
        (let ((al (map cons (list (string->symbol "th") (string->symbol "om")) s)))
          (make-bytes 256)                     ; keep the collector busy
          (loop (+ i 1) (list (+ 1.0 i) (+ 2.0 i))
                (if (and (= (length al) 2)
                         (pair? (car al)) (pair? (cadr al))
                         (symbol? (car (car al))) (number? (cdr (cadr al))))
                    bad
                    (+ bad 1)))))))

;; It used to fail every 1621 iterations, exactly — a fixed allocation per
;; pass puts the collection at the same point in the cycle each time — so
;; ten thousand is many times over the old threshold.
(assert-equal "n-ary map keeps a subr's result across the cons that stores it"
              0 (gc-map-nary 10000))

(define (gc-map-2 n)
  (let loop ((i 0) (bad 0))
    (if (= i n)
        bad
        (let ((al (map list (list (+ 0.0 i) (+ 1.0 i) (+ 2.0 i)))))
          (make-bytes 256)
          (loop (+ i 1) (if (and (= (length al) 3) (pair? (car al))) bad (+ bad 1)))))))

(assert-equal "and so does the two-argument path"
              0 (gc-map-2 10000))

(suite-summary)
