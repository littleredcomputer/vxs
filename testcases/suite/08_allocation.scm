;;----------------------------------------------------------------------
;; Layer 08: allocation invariants
;;
;; These are PERFORMANCE regressions expressed as tests. They exist
;; because the costs they guard were invisible until (vm-stats) made
;; them countable — at which point a twenty-year-old decision to compile
;; `let` as an immediately-applied lambda turned out to be the single
;; largest allocation source in ordinary Scheme code (the particle-fiber
;; demo was burning 735 objects and ~2.3 MB per frame; it is now zero).
;;
;; The property worth protecting is narrow and valuable: a steady-state
;; animation loop that builds no aggregates allocates NOTHING, so the
;; collector never runs during animation. Every zero below is part of
;; that guarantee. If one turns non-zero, some form quietly started
;; heap-allocating again and frame-time jitter follows.
;;
;; Only ZERO is asserted, never an exact non-zero count — an optimization
;; that lowers a cost should not fail its own test.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")

(test-suite "08_allocation: zero-allocation invariants")

(define (objects-allocated)
  (cdr (assq 'total-objects-allocated (vm-stats))))

;; Objects attributable to ONE iteration. Run at two sizes and difference
;; them so the harness's own cost (vm-stats builds an alist) and any
;; one-time setup cancel exactly. Cumulative counters never decrease, so
;; a GC landing mid-measurement cannot perturb this.
(define (alloc-per-iter run n)
  (run 50)                                   ; warm / force lazy init
  (let* ((a0 (objects-allocated)) (r1 (run n))       (a1 (objects-allocated))
         (b0 (objects-allocated)) (r2 (run (* 2 n))) (b1 (objects-allocated)))
    (/ (- (- b1 b0) (- a1 a0)) n)))

(define ITERS 2000)
(define (zero-alloc name run)
  (assert-equal name 0 (alloc-per-iter run ITERS)))

;; --- binding forms ------------------------------------------------------

(define (p-let x)  (let ((a (* x 2.0))) a))
(define (p-let3 x) (let ((a (* x 2.0)) (b (* x 3.0)) (c (* x 4.0))) (+ a b c)))
(define (p-let* x) (let* ((a (* x 2.0)) (b (+ a 1.0)) (c (+ b 1.0))) c))
(define (p-let-if x k)
  (let ((a (if (= k 0) 0.5 0.0)) (b (if (= k 1) 0.5 0.0))) (+ x a b)))

(define (r-bare n)   (let loop ((i 0) (a 0.0)) (if (= i n) a (loop (+ i 1) a))))
(define (r-let n)    (let loop ((i 0) (a 0.0)) (if (= i n) a (loop (+ i 1) (+ a (p-let 1.5))))))
(define (r-let3 n)   (let loop ((i 0) (a 0.0)) (if (= i n) a (loop (+ i 1) (+ a (p-let3 1.5))))))
(define (r-let* n)   (let loop ((i 0) (a 0.0)) (if (= i n) a (loop (+ i 1) (+ a (p-let* 1.5))))))
(define (r-let-if n) (let loop ((i 0) (a 0.0)) (if (= i n) a (loop (+ i 1) (+ a (p-let-if 1.5 1))))))

(zero-alloc "named-let loop iteration allocates nothing" r-bare)
(zero-alloc "(let) with 1 binding allocates nothing"     r-let)
(zero-alloc "(let) with 3 bindings allocates nothing"    r-let3)
(zero-alloc "(let*) with 3 bindings allocates nothing"   r-let*)
(zero-alloc "(let) with if-valued bindings allocates nothing" r-let-if)

;; --- calls --------------------------------------------------------------

(define (f3 a b c) (+ a b c))
(define (apply2 g x) (g x))
(define (dbl x) (* 2.0 x))
(define (sum-down k) (if (= k 0) 0 (+ k (sum-down (- k 1)))))

(define (r-call n)  (let loop ((i 0) (a 0.0)) (if (= i n) a (loop (+ i 1) (+ a (f3 1.0 2.0 3.0))))))
(define (r-subr n)  (let loop ((i 0) (a 0.0)) (if (= i n) a (loop (+ i 1) (+ a (sqrt 4.0))))))
(define (r-vsubr n) (let loop ((i 0) (a 0.0)) (if (= i n) a (loop (+ i 1) (+ a 1.0 2.0 3.0 4.0 5.0)))))
(define (r-hof n)   (let loop ((i 0) (a 0.0)) (if (= i n) a (loop (+ i 1) (+ a (apply2 dbl 1.5))))))
(define (r-deep n)  (let loop ((i 0) (a 0))   (if (= i n) a (loop (+ i 1) (+ a (sum-down 20))))))

(zero-alloc "fixed-arity closure call allocates nothing"   r-call)
(zero-alloc "native subr call allocates nothing"           r-subr)
(zero-alloc "variable-arity SUBR call allocates nothing"   r-vsubr)
(zero-alloc "higher-order call allocates nothing"          r-hof)
;; Frames live on a deque, not the heap — so ordinary (non-tail) recursion
;; is allocation-free too. This is separate from TCO, which saves depth.
(zero-alloc "non-tail recursion 20 deep allocates nothing" r-deep)

;; --- data access --------------------------------------------------------

(define scratch (make-vector 32 0.5))
(define (r-vref n) (let loop ((i 0) (a 0.0)) (if (= i n) a (loop (+ i 1) (+ a (vector-ref scratch 7))))))
(define (r-vset n) (let loop ((i 0) (a 0.0)) (if (= i n) a (begin (vector-set! scratch 7 0.5) (loop (+ i 1) a)))))
(define (r-arith n) (let loop ((i 0) (a 0.0)) (if (= i n) a (loop (+ i 1) (+ a (* 1.5 (sin 0.5)) (- 2.0 1.0))))))

(zero-alloc "vector-ref allocates nothing"  r-vref)
(zero-alloc "vector-set! allocates nothing" r-vset)
;; Numbers are NaN-boxed inline, so float arithmetic never reaches the heap.
(zero-alloc "float arithmetic allocates nothing" r-arith)

;; --- the composite property this all exists for -------------------------
;; A frame-loop body doing real work over an existing buffer, touching no
;; aggregates: this is the shape that must stay at zero forever.

(define pts (make-vector 64 0.0))
(define (frame-body n)
  (let loop ((i 0) (acc 0.0))
    (if (= i n)
        acc
        (let* ((slot (modulo i 64))
               (x (vector-ref pts slot))
               (nx (+ x (* 0.01 (sin (* i 0.01))))))
          (vector-set! pts slot nx)
          (loop (+ i 1) (+ acc nx))))))
(zero-alloc "steady-state frame loop over a buffer allocates nothing" frame-body)

;; --- string building ----------------------------------------------------
;; A string port's buffer grows in C++ storage, not the Scheme heap, so
;; emitting a page of source text costs a CONSTANT number of heap objects
;; regardless of length. string-append accumulation costs one object per
;; append and copies quadratically. This is the difference between shader
;; codegen being invisible to the collector and being a GC event.

(define (append-build n)
  (let loop ((i 0) (s ""))
    (if (= i n) (string-length s) (loop (+ i 1) (string-append s "line;\n")))))

(define (port-build n)
  (let ((p (open-output-string)))
    (let loop ((i 0))
      (if (= i n)
          (string-length (get-output-string p))
          (begin (display "line;\n" p) (loop (+ i 1)))))))

(define (objects-for f n)
  (f 20)                                     ; warm
  (let* ((a0 (objects-allocated)) (r (f n)) (a1 (objects-allocated)))
    (- a1 a0)))

;; The invariant is that the port's cost does not GROW with output size —
;; asserted as a comparison rather than an exact count, so an unrelated
;; change in how many objects a port needs cannot make this brittle.
(assert-equal "a string port's object cost is flat in output size"
              #t
              (= (objects-for port-build 400) (objects-for port-build 1600)))
(assert-equal "string-append's object cost grows with output size"
              #t
              (> (objects-for append-build 1600) (objects-for append-build 400)))

;; --- the documented exception -------------------------------------------
;; Variadic CLOSURES cons their rest list (one cons per rest arg); variadic
;; subrs do not, because they receive a Value* into the stack instead. This
;; asymmetry is deliberate and known — asserted loosely so that eliminating
;; it later is an improvement, not a failure.

(define (fvar . args) (car args))
(define (r-var n) (let loop ((i 0) (a 0.0)) (if (= i n) a (loop (+ i 1) (+ a (fvar 1.0 2.0 3.0))))))
(assert-equal "variadic closure conses its rest list (known, deliberate)"
              #t
              (> (alloc-per-iter r-var ITERS) 0))

;;--- the byte counter must not underflow ---------------------------------
;; bytes_allocated is UNSIGNED, and the sweep used to decrement it once per
;; corpse. That is wrong for any object whose storage arrives after the
;; object does: make-bytes allocates an empty ObjBytes and then sizes its
;; vector, so allocation charged for the header alone while the sweep
;; credited back the header PLUS the data. A one-megabyte buffer added
;; 1,248 bytes to the counter and removed 1,001,248 of them, and the
;; difference wrapped.
;;
;; It surfaced as a heap of 4,187,859 KB in the browser — 32-bit wasm, so it
;; wrapped at 2^32 — while only 622 objects were live. The sweep now
;; recomputes from the survivors instead, which cannot underflow and is
;; exact rather than cumulative. ObjMap had the identical shape.

(define (live-bytes) (cdr (assq 'live-bytes (vm-stats))))

(define alloc-baseline (begin (gc) (live-bytes)))
(define big (make-bytes 1000000))
(assert-true "a large buffer is actually charged for"
             (> (- (live-bytes) alloc-baseline) 900000))
(set! big #f)
(gc)
(define alloc-after (live-bytes))
;; The load-bearing assertion: an underflowed counter is enormous, so any
;; sane upper bound catches it.
(assert-true "the counter does not underflow when the buffer is collected"
             (< alloc-after (+ alloc-baseline 100000)))
(assert-true "and the space is actually reclaimed"
             (< alloc-after (+ alloc-baseline 50000)))

;; A map grows its entries vector long after the object exists — same shape.
(define m (hash-map))
(let fill ((i 0))
  (if (< i 2000) (begin (map-set! m i i) (fill (+ i 1)))))
(set! m #f)
(gc)
(assert-true "nor when a grown map is collected"
             (< (live-bytes) (+ alloc-baseline 100000)))


;;--- map-copy and map-delete! -------------------------------------------
;; Scheme's convention is that aggregates are mutable and you copy
;; explicitly — vector-copy, string-copy, list-copy. Maps had no -copy,
;; which meant handing a map out of a structure handed out the structure's
;; own storage: an edit to what looked like a candidate silently edited
;; the original.

(define original {:x 1 :y 2})
(define candidate (map-copy original))
(map-set! candidate :x 99)
(map-delete! candidate :y)
(assert-equal "the copy can be edited freely" '(99 #f)
              (list (map-ref candidate :x) (map-has? candidate :y)))
(assert-equal "and the original is untouched" '(1 #t)
              (list (map-ref original :x) (map-has? original :y)))

(assert-equal "a copy is a different object" #f (eq? original (map-copy original)))
(assert-equal "with the same contents"
              '(1 2) (let ((c (map-copy original))) (list (map-ref c :x) (map-ref c :y))))

(assert-equal "deleting an absent key does nothing"
              1 (let ((m {:a 1})) (map-delete! m :nope) (map-count m)))
(assert-equal "deleting the last key leaves an empty map"
              0 (let ((m {:a 1})) (map-delete! m :a) (map-count m)))
(assert-equal "and the key is really gone"
              #f (let ((m {:a 1 :b 2})) (map-delete! m :a) (map-has? m :a)))
(assert-equal "while its neighbours survive"
              2 (let ((m {:a 1 :b 2})) (map-delete! m :a) (map-ref m :b)))

;; SHALLOW, like every other -copy in Scheme. Worth pinning rather than
;; assuming, because a structure of nested maps — the obvious shape for
;; anything hierarchical — is only protected one level deep, and the
;; sharing is invisible until something writes through it.
(define outer {:inner {:v 1}})
(define shallow (map-copy outer))
(map-set! (map-ref shallow :inner) :v 42)
(assert-equal "map-copy is shallow: a nested map is shared"
              42 (map-ref (map-ref outer :inner) :v))

(assert-equal "map-copy refuses a non-map"
              'raised (guard (e (#t 'raised)) (map-copy 5)))

(suite-summary)
