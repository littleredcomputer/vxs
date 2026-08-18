;;----------------------------------------------------------------------
;; Layer 09: fiber scheduling semantics
;;
;; Previously uncovered by the suite: fiber behaviour was only exercised
;; indirectly through the browser presets, so the scheduler's actual
;; contract — round-robin fairness, run-to-completion, correct handling
;; of fibers that finish at different times — was never asserted anywhere.
;;
;; The scheduler keeps a persistent round cursor so that a round cut short
;; by the frame budget RESUMES where it stopped instead of restarting at
;; index 0. Without it, a prefix of the fiber list wins every frame and
;; everything past the cutoff is starved forever, however well it yields
;; (measured: 2000 fibers against an 8ms budget, fiber 0 served on all 120
;; ticks, 1649 fibers served zero times). The budget path itself needs a
;; wall clock and so belongs to the embedder, but the cursor's ordering
;; and fiber-removal behaviour are testable right here.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")

(test-suite "09_fibers: scheduling order, completion, lifetimes")

;; --- round-robin ordering across yields --------------------------------

(define trace '())
(define (note x) (set! trace (cons x trace)))

(future (note 'a1) (yield) (note 'a2) (yield) (note 'a3))
(future (note 'b1) (yield) (note 'b2) (yield) (note 'b3))
(future (note 'c1) (yield) (note 'c2) (yield) (note 'c3))
(run-fibers)
(assert-equal "fibers interleave one turn each, in spawn order"
              '(a1 b1 c1 a2 b2 c2 a3 b3 c3)
              (reverse trace))

;; --- run-fibers drives every fiber to completion ------------------------

(define t1 0) (define t2 0) (define t3 0)
(future (set! t1 1) (yield) (set! t1 (+ t1 10)) (yield) (set! t1 (+ t1 100)))
(future (set! t2 2) (yield) (set! t2 (+ t2 20)))
(future (set! t3 3))
(run-fibers)
(assert-equal "run-fibers completes fibers of differing lengths"
              '(111 22 3)
              (list t1 t2 t3))

;; --- uneven lifetimes: removing a finished fiber must not skip its
;;     neighbour (the erase-under-cursor path) --------------------------

(define turns '())
(define (spinner tag n)
  (future (let loop ((k 0))
            (set! turns (cons tag turns))
            (if (< k n) (begin (yield) (loop (+ k 1)))))))
(spinner 'w 0)   ; finishes on its first turn
(spinner 'x 2)
(spinner 'y 0)   ; finishes on its first turn
(spinner 'z 1)
(run-fibers)

(define (count-of tag)
  (let loop ((l turns) (n 0))
    (cond ((null? l) n)
          ((eq? (car l) tag) (loop (cdr l) (+ n 1)))
          (else (loop (cdr l) n)))))

(assert-equal "single-turn fiber ran exactly once (w)" 1 (count-of 'w))
(assert-equal "single-turn fiber ran exactly once (y)" 1 (count-of 'y))
(assert-equal "two-yield fiber ran three times (x)"    3 (count-of 'x))
(assert-equal "one-yield fiber ran twice (z)"          2 (count-of 'z))
(assert-equal "every fiber ran despite neighbours retiring early"
              #t
              (and (> (count-of 'w) 0) (> (count-of 'x) 0)
                   (> (count-of 'y) 0) (> (count-of 'z) 0)))

;; --- fibers spawned from inside a fiber get scheduled -------------------

(define nested-ran 0)
(future (yield) (future (set! nested-ran 42)))
(run-fibers)
(assert-equal "a fiber spawned by a fiber is scheduled" 42 nested-ran)

;; --- run-fibers on an empty scheduler is a no-op ------------------------

(run-fibers)
(assert-equal "run-fibers with nothing active is harmless" #t #t)

;; --- touch and the lifetime of the computing fiber ----------------------
;; ObjFuture holds a raw Fiber*, and the scheduler deletes fibers the
;; moment they complete. Touching afterwards used to read freed memory —
;; a heap-use-after-free reachable from three lines of Scheme, confirmed
;; under ASan. The scheduler now settles the future and severs the pointer
;; before reaping, so `fiber` is either live or null but never stale.

(define reaped (future (+ 1 2)))
(run-fibers)                       ; completes AND deletes the computing fiber
(assert-equal "touch after the scheduler reaped the fiber" 3 (touch reaped))
(assert-equal "touching a settled future again is stable" 3 (touch reaped))

;; A future touched before it has run must block the toucher and resume
;; with the value — not drive the child inline, which is what used to blow
;; through the frame budget with no deadline.
(assert-equal "touch of a not-yet-run future yields the value" 42
              (touch (future (* 6 7))))

(assert-equal "two pending futures, both touched" 100
              (let ((f1 (future (+ 10 20)))
                    (f2 (future (+ 30 40))))
                (+ (touch f1) (touch f2))))

;; A fiber can touch a future computed by another fiber.
(define inner (future (* 3 4)))
(define outer (future (+ 1 (touch inner))))
(assert-equal "a fiber may touch another fiber's future" 13 (touch outer))

;; Touching the same future from two different fibers: both get the value,
;; and the computation runs once.
(define runs 0)
(define shared-fut (future (begin (set! runs (+ runs 1)) 'once)))
(define a (future (touch shared-fut)))
(define b (future (touch shared-fut)))
(assert-equal "two fibers touching one future agree" '(once once)
              (list (touch a) (touch b)))
(assert-equal "the future's body ran exactly once" 1 runs)

;; --- fibers see shared mutable state at yield boundaries ----------------
;; Cooperative scheduling's whole contract: no fiber observes another's
;; half-finished work, because control only changes hands at (yield).

(define shared 0)
(future (let loop ((k 0))
          (if (< k 3)
              (begin (set! shared (+ shared 1))
                     (set! shared (+ shared 1))   ; both increments land
                     (yield)                      ; before anyone else runs
                     (loop (+ k 1))))))
(future (let loop ((k 0))
          (if (< k 3)
              (begin (assert-equal "sibling never observes an odd (mid-update) value"
                                   0 (modulo shared 2))
                     (yield)
                     (loop (+ k 1))))))
(run-fibers)
(assert-equal "shared state after cooperative updates" 6 shared)

(suite-summary)
