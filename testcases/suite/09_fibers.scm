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

;; --- the scheduler is RE-ENTRANT, and must survive being so -------------
;;
;; A fiber that touches a fiber-backed future from somewhere it cannot
;; suspend — inside guard, map, apply — cannot block, so the VM rescues it
;; by pumping the whole scheduler from inside that fiber's own step. The
;; nested round appends fibers, retires others, and moves the round cursor.
;;
;; Both scheduler call sites used to retire a finished fiber by a POSITION
;; recorded before it ran, and after a nested round that position meant
;; nothing. erase() past the end computes a negative move size: SIGBUS
;; natively, out-of-bounds in wasm. Not a raise anyone could catch — the
;; whole VM went down, from four lines of ordinary Scheme.
;;
;; These assertions are cheap; what they are really testing is that the
;; process is still alive to make them.

(define g (future (guard (e (#t 'caught)) (touch (future 42)))))
(assert-equal "guard + touch of a fiber-backed future survives" 42 (touch g))

(define g2 (future (guard (e (#t (quote x)))
                    (touch (future (guard (e (#t (quote y)))
                                     (touch (future 7))))))))
(assert-equal "and nests" 7 (touch g2))

(define g3 (future (car (map (lambda (x) (touch (future x))) (list 1 2 3)))))
(assert-equal "so does touch inside map" 1 (touch g3))

;; With enough siblings that the fiber vector genuinely reallocates and
;; genuinely shifts under the nested round, rather than happening to sit
;; still because it was small.
(do ((i 0 (+ i 1))) ((= i 20))
  (future (let lp ((n 0)) (if (< n 3) (begin (yield) (lp (+ n 1)))))))
(define g4 (future (guard (e (#t 'c)) (touch (future 99)))))
(assert-equal "with twenty siblings churning underneath" 99 (touch g4))
(run-fibers)


;;--- generators: a fiber driven by hand ---------------------------------
;; The other thing a fiber can be. A future is undirected and settles
;; ONCE; a generator is directed and many-shot. They share a Fiber and
;; almost nothing else, which is why `touch` was not given a second
;; argument to do this job.

(define g (generator (lambda () (yield 1) (yield 2) 'done)))
(assert-equal "a generator yields its first value" 1 (resume g))
(assert-equal "and its second" 2 (resume g))
(assert-equal "it is live until the thunk returns" #t (generator-live? g))
(assert-equal "the last resume gives the RETURN value" 'done (resume g))
(assert-equal "and then it is finished" #f (generator-live? g))
(assert-equal "resuming a finished generator is an error"
              'raised (guard (e (#t 'raised)) (resume g)))

;; Nothing round-robins a generator: it is not in active_fibers, so it
;; makes no progress at all unless someone resumes it. Without that, a
;; (run-fibers) between resumes would advance it behind the caller's back.
(define counter 0)
(define quiet (generator (lambda () (set! counter 1) (yield 'a) (set! counter 2) 'b)))
(run-fibers)
(assert-equal "an unresumed generator has not started" 0 counter)
(resume quiet)
(run-fibers)
(assert-equal "and does not advance on its own between resumes" 1 counter)

;; The half that makes it a conversation rather than a pump: (yield v) is
;; an EXPRESSION, and its value is whatever the resumer passed back.
(define echo (generator (lambda ()
                          (let loop ((got (yield 'ready)))
                            (loop (yield (list 'saw got)))))))
(assert-equal "the first resume gets what the generator offered"
              'ready (resume echo))
(assert-equal "and a value sent in comes back out"
              '(saw apple) (resume echo 'apple))
(assert-equal "again, with a different value"
              '(saw pear) (resume echo 'pear))
(assert-equal "a resume with no value leaves yield unspecified"
              #t (let ((r (resume echo))) (equal? r (list 'saw (if #f #f)))))

;; Bare (yield) still works and still means "hand out nothing" — the
;; demos are full of it, and it must not start seeing a stale value from
;; some earlier yield.
(define mixed (generator (lambda () (yield 7) (yield) 'end)))
(assert-equal "a valued yield hands out its value" 7 (resume mixed))
(assert-equal "a bare yield hands out nothing"
              #t (equal? (resume mixed) (if #f #f)))

;; Failure modes. Each of these would otherwise be a hang or a crash.
(define self #f)
(set! self (generator (lambda () (resume self) 'never)))
(assert-equal "a generator cannot resume itself"
              'raised (guard (e (#t 'raised)) (resume self)))

(define boom (generator (lambda () (yield 1) (car '()) 2)))
(resume boom)
(assert-equal "an error in the body reaches the resumer"
              'raised (guard (e (#t 'raised)) (resume boom)))
(assert-equal "and finishes the generator" #f (generator-live? boom))

;; A generator is driven only by resume, so nothing will ever settle a
;; future it waits on. That is a hang, not a suspension, and it is worth
;; more than the stale unspecified the naive reading would return.
(define blocked (generator (lambda () (touch (future 42)))))
(assert-equal "waiting on a future from a generator is refused"
              'raised (guard (e (#t 'raised)) (resume blocked)))

(assert-equal "generator? distinguishes them from futures"
              '(#t #f #f) (list (generator? mixed) (generator? (future 1)) (generator? 5)))

;; A KNOWN BOUNDARY, pinned so it is a documented limit rather than a
;; surprise: (yield) inside guard fails in a generator for exactly the
;; reason (touch) does — guard's continuation includes native C++ frames,
;; so the fiber cannot suspend through it. Anything driving a generator
;; must keep its yields outside guard, which is a real constraint on how
;; a backtracking search built on this has to be shaped.
(define guarded (generator (lambda () (guard (e (#t 'c)) (yield 'x)) 'end)))
(assert-equal "yielding from inside guard is refused, not silently wrong"
              'raised (guard (e (#t 'raised)) (resume guarded)))

;; Abandoned mid-run, and therefore collected with a live fiber inside.
;; The generator OWNS that fiber — unlike a future, whose fiber the
;; scheduler roots until it completes — so nothing else can free it.
;;
;; The assertion is about COLLECTION PRESSURE, not just survival. A
;; SlabStack allocates 32KB the moment it exists, and an ObjGenerator is
;; about forty bytes, so a collector charged only for the object sees no
;; reason to run while an unbounded amount of fiber piles up behind it.
;; Measured before make_generator charged for the fiber: 20,000 abandoned
;; generators peaked at 222MB of RSS across FOUR collections. After: 2.4MB.
;; "Did not crash" would have passed both, which is why this counts GCs.
(define (stat name) (cdr (assq name (vm-stats))))
(define gc-before (stat 'gc-count))
(do ((i 0 (+ i 1))) ((= i 3000))
  (resume (generator (lambda () (yield i) i))))
(assert-equal "abandoning generators provokes collection, not just growth"
              #t (> (stat 'gc-count) gc-before))
(gc)
(assert-equal "and they are genuinely reclaimed"
              #t (< (stat 'live-bytes) (* 4 1024 1024)))

(suite-summary)
