;;----------------------------------------------------------------------
;; Layer 19: the point pool (lib/actors.scm)
;;
;; A fiber per actor only works if an actor can OWN a slot in the point
;; buffer — hold it for its life and hand it back when it dies. Without
;; that, a dead actor's last position sits on screen forever and a newborn
;; has nowhere to go, so births and deaths become decorative rather than
;; real. The pool is that ownership, and it is a free list over indices.
;;
;; The subtle half is that releasing must also BLANK. A point of size zero
;; produces a degenerate quad — six coincident vertices, no fragments — so
;; a blanked slot is invisible rather than merely unowned. That is what
;; lets the draw always submit the full capacity without knowing or caring
;; how many actors are alive. Release and blank are one operation here
;; precisely so that no caller can do one and forget the other.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")
(load "lib/actors.scm")

(test-suite "19_actors: point ownership for fiber-per-agent")

(define p (make-point-pool 4))

;;--- shape --------------------------------------------------------------

(assert-equal "capacity is remembered" 4 (pool-capacity p))
(assert-equal "nothing is live to begin with" 0 (pool-live p))
(assert-equal "the buffer is sized for the capacity"
              (* 4 points-stride 4) (bytes-length (pool-bytes p)))
(assert-true "the view covers it" (= 28 (view-length (pool-view p))))

;;--- claiming -----------------------------------------------------------
;; Ascending, so the first actor gets slot 0. Nothing depends on the order,
;; but an ascending free list makes a buffer dump readable while debugging,
;; and the comment beside it said so before the code did — the first
;; version handed out 3, 2, 1, 0 and this assertion is why that was caught.

(assert-equal "claims come out ascending" '(0 1 2) (list (pool-claim! p)
                                                          (pool-claim! p)
                                                          (pool-claim! p)))
(assert-equal "live tracks the claims" 3 (pool-live p))
(assert-equal "the last slot" 3 (pool-claim! p))

;; A full pool declines rather than raising. Running out of room is an
;; ordinary condition for a spawner — it should skip a birth this frame,
;; not die — so #f is the answer and an error would be wrong.
(assert-equal "a full pool returns #f, it does not raise" #f (pool-claim! p))
(assert-equal "and stays full" 4 (pool-live p))

;;--- writing and releasing ----------------------------------------------

(pool-write! p 1 0.5 0.25 -0.5 0.03125 1.0 0.5 0.25)
(assert-equal "an actor writes its own point" 0.5 (point-x (pool-view p) 1))
(assert-equal "including z" -0.5 (point-z (pool-view p) 1))

(pool-release! p 1)
(assert-equal "release blanks the position" 0.0 (point-x (pool-view p) 1))
(assert-equal "and the depth" 0.0 (point-z (pool-view p) 1))
;; THE assertion of this file: size 0 is what makes the slot invisible.
;; A released slot that kept its size would draw a stale corpse every frame
;; and nothing else in the system would object.
(assert-equal "and the SIZE, which is what makes it invisible"
              0.0 (view-ref (pool-view p) (+ 3 (* 1 points-stride))))
(assert-equal "live drops" 3 (pool-live p))

(assert-equal "the freed slot is handed out again" 1 (pool-claim! p))
(assert-equal "and live rises" 4 (pool-live p))

;;--- neighbours are undisturbed -----------------------------------------
;; An off-by-one in the stride would blank the wrong actor, and it would
;; look like an unrelated agent dying at random.

(pool-write! p 0 0.75 0.5 0.25 0.0625 1.0 1.0 1.0)
(pool-write! p 2 -0.75 -0.5 -0.25 0.0625 1.0 1.0 1.0)
(pool-release! p 0)
(assert-equal "releasing 0 leaves 2 alone" -0.75 (point-x (pool-view p) 2))
(assert-equal "and its size" 0.0625
              (view-ref (pool-view p) (+ 3 (* 2 points-stride))))

;;--- a pool of nothing --------------------------------------------------
(define empty (make-point-pool 0))
(assert-equal "a zero-capacity pool claims nothing" #f (pool-claim! empty))
(assert-equal "and has an empty buffer" 0 (bytes-length (pool-bytes empty)))

(suite-summary)
