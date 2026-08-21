;;----------------------------------------------------------------------
;; Actors: a fiber per agent, writing into the GPU point buffer
;;
;; The point of this file is small and specific. A fiber earns its keep
;; only when the thing it models has private evolving state AND its own
;; control flow — an actor that knows things and wants things has both, a
;; bouncing ball has neither, and "all you really need for that is an
;; array" is a complete refutation of the second case. So: one fiber per
;; actor, each closing over whatever it needs, each writing its own point.
;;
;; What was missing was ownership. An actor has to claim a slot in the
;; buffer, keep it for its life, and give it back when it dies — otherwise
;; a dead actor's last position sits on screen forever and a new one has
;; nowhere to go. That is all a POOL is: a free list over point indices.
;;
;; A released slot is zeroed, which makes it invisible rather than merely
;; unowned: a point of size 0 produces a degenerate quad, all six vertices
;; coincident, so it rasterizes no fragments at all. That is why the draw
;; can always submit the full capacity and never needs to know how many
;; actors are currently alive.
;;----------------------------------------------------------------------

(load "lib/points.scm")

;; (make-point-pool capacity) -> pool
;; A pool is (bytes view free-list live-count capacity).
(define (make-point-pool capacity)
  (let ((buf (make-points capacity)))
    (vector buf
            (points-view buf)
            ;; Free indices, highest first, so the first claims are 0, 1, 2 —
            ;; tidier to reason about when debugging.
            (let build ((i (- capacity 1)) (acc '()))
              (if (< i 0) acc (build (- i 1) (cons i acc))))
            0
            capacity)))

(define (pool-bytes p)    (vector-ref p 0))
(define (pool-view p)     (vector-ref p 1))
(define (pool-live p)     (vector-ref p 3))
(define (pool-capacity p) (vector-ref p 4))

;; Take a slot, or #f when the pool is full. An actor that cannot claim one
;; should decline to exist rather than share, which is why this returns #f
;; instead of raising: running out of room is an ordinary condition here,
;; not a bug.
(define (pool-claim! p)
  (let ((free (vector-ref p 2)))
    (if (null? free)
        #f
        (begin
          (vector-set! p 2 (cdr free))
          (vector-set! p 3 (+ (pool-live p) 1))
          (car free)))))

;; Give a slot back, and blank it in the same breath. Doing both here is
;; deliberate: a release that forgot to zero would leave the corpse on
;; screen, and the two would then have to be remembered together at every
;; call site.
(define (pool-release! p i)
  (point-set! (pool-view p) i 0.0 0.0 0.0 0.0 0.0 0.0 0.0)
  (vector-set! p 2 (cons i (vector-ref p 2)))
  (vector-set! p 3 (- (pool-live p) 1)))

;; (pool-write! pool i x y z size r g b) — an actor writing its own point.
(define (pool-write! p i x y z size r g b)
  (point-set! (pool-view p) i x y z size r g b))
