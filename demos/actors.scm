;;; ==========================================================
;;; Actors — one fiber per agent, writing into the GPU buffer
;;; ==========================================================
;;; Drag to orbit, scroll to zoom.
;;;
;;; Every dot is a FIBER with its own state and its own control flow. It is
;;; born, it wanders, it spends energy deciding where to go, and when the
;;; energy runs out it dies and hands its slot back. Nothing steps a list
;;; of particles; each actor runs its own loop and yields once per frame.
;;;
;;; Colour is age. A new actor is white-hot; as its energy drains it cools
;;; through amber and red to a dim violet, and then it is gone. Thinking
;;; costs more energy than coasting, so the restless ones cool fastest.
;;;
;;; That distinction is the whole point. A fiber per bouncing ball proves
;;; nothing — an array does that better. A fiber earns its keep when the
;;; thing it models has private evolving state AND its own control flow,
;;; which an agent with intent has and a ball has not.
;;;
;;; It is also the thing a fragment shader cannot do. A shader is a pure
;;; function of position with no memory, no per-entity control flow, and no
;;; way to spawn or retire anything. State would have to be encoded into
;;; textures and ping-ponged, and birth and death are where that model
;;; gives up entirely. Here the CPU decides and the GPU draws.

(load "lib/gpu.scm")
(load "lib/actors.scm")
(load "lib/threefry.scm")

(define CAPACITY 900)
(define pool (make-point-pool CAPACITY))
(define cam (make-camera))
(define key (vector 20260821 0 0 0))

;;; Randomness indexed by (actor id, decision number) rather than drawn
;;; from a stream — so an actor's choices depend only on who it is and how
;;; many decisions it has made, never on how many other actors exist or on
;;; the order the scheduler happened to run them in. With hundreds of
;;; fibers interleaving, a shared stream would make every run different and
;;; none of them reproducible.
(define (decide id n)
  (threefry4x32-unit (vector id n 0 0) key))

;;; One actor. Private state lives in the loop variables — position,
;;; heading, energy, and how many decisions it has taken — and is reachable
;;; from nowhere else in the system.
(define (spawn-actor! id)
  (let ((slot (pool-claim! pool)))
    (if slot
        (future
          (let ((r0 (decide id 0)))
            (let loop ((x 0.0) (y 0.0) (z 0.0)
                       (hx (- (vector-ref r0 0) 0.5))
                       (hy (* 0.4 (- (vector-ref r0 1) 0.5)))
                       (hz (- (vector-ref r0 2) 0.5))
                       (energy 1.0)
                       (n 1))
              (if (<= energy 0.0)
                  ;; Death: give the slot back. The release blanks the point,
                  ;; so the actor leaves no corpse on screen.
                  (pool-release! pool slot)
                  (let* ((think? (= 0 (modulo n 24)))
                         (r (if think? (decide id n) #f))
                         ;; Deciding costs energy. An actor that changes its
                         ;; mind often does not live as long as one that
                         ;; commits — which is the cheapest possible way to
                         ;; give a choice a consequence.
                         (nhx (if think? (+ (* 0.6 hx) (- (vector-ref r 0) 0.5)) hx))
                         (nhy (if think? (+ (* 0.6 hy) (* 0.4 (- (vector-ref r 1) 0.5))) hy))
                         (nhz (if think? (+ (* 0.6 hz) (- (vector-ref r 2) 0.5)) hz))
                         (cost (if think? 0.010 0.0035))
                         (nx (+ x (* 0.012 nhx)))
                         (ny (+ y (* 0.012 nhy)))
                         (nz (+ z (* 0.012 nhz)))
                         ;; A soft wall: heading away from the origin costs
                         ;; more, so the swarm stays roughly bounded without
                         ;; anything being clamped.
                         (rad (sqrt (+ (* nx nx) (* ny ny) (* nz nz))))
                         (drag (if (> rad 0.9) 0.010 0.0))
                         (e (- energy cost drag)))
                    ;; Heat death: an actor is white-hot when it is new and
                    ;; cools through amber, red and magenta to a dim violet
                    ;; as its energy goes. The same ramp the GPU wrangle
                    ;; uses, evaluated here because the actor picks its own
                    ;; colour — and read a channel at a time, since a fresh
                    ;; colour vector per actor per frame would have been the
                    ;; largest allocation source in the program.
                    (pool-write-heat! pool slot nx ny nz
                                      (+ 0.003 (* 0.011 e)) e)
                    (yield)
                    (loop nx ny nz nhx nhy nhz e (+ n 1))))))))))

;;; A fiber whose whole job is making more fibers. Population is dynamic:
;;; actors die on their own schedule and the spawner refills, so the count
;;; settles wherever birth and death balance rather than being chosen.
(define next-id 0)
(define BIRTHS-PER-FRAME 2)
(future
  (let loop ()
    (let born ((k 0))
      (if (and (< k BIRTHS-PER-FRAME) (< (pool-live pool) CAPACITY))
          (begin (spawn-actor! next-id)
                 (set! next-id (+ next-id 1))
                 (born (+ k 1)))))
    (yield)
    (loop)))

;;; The renderer is just another fiber. It does not know what actors are —
;;; it draws whatever is in the buffer.
(run-points-loop (pool-bytes pool) CAPACITY
                 (lambda (t) (orbit-camera! cam))
                 cam "vxs-gpu-canvas")
