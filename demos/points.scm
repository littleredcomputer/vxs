;;; ==========================================================
;;; GPU Instanced Points — a cube of points in 3D. Drag to orbit.
;;; ==========================================================
;;; Scheme fills a buffer; the GPU draws one quad per point. Seven floats
;;; each — x, y, z, size, r, g, b — flat rather than an array of structs,
;;; because WGSL gives vec3<f32> a 16-byte alignment inside a storage
;;; array and a struct would not pack the way these fields pack here.
;;;
;;; The camera is PARAMETERS, not a matrix: yaw, pitch, distance and fov
;;; ride in the uniform and the vertex shader rotates and projects. There
;;; is no depth buffer either — additive blending is commutative, so a
;;; glowing point cloud is order-independent and never needs sorting.
;;;
;;; Scheme rewrites every point each frame. Watch the obj/f and GC
;;; counters: that CPU cost is what a compute wrangle will delete.

(load "lib/gpu.scm")
(load "lib/threefry.scm")

(define SIDE 14)                       ; SIDE^3 points
(define N (* SIDE SIDE SIDE))
(define buf (make-points N))
(define pts (points-view buf))
(define cam (make-camera))
(define key (vector 0 0 0 0))

(define (grid-coord i) (- (* 2.0 (/ i (- SIDE 1.0))) 1.0))

;;; Base positions and hues are computed ONCE. They do not change between
;;; frames, and working them out per frame — three integer divisions and
;;; three grid-coord calls per point — cost about 40% of the pass for
;;; nothing. What remains in update! is only what actually varies with
;;; time.
;;;
;;; One Threefry block per point gives four independent randoms: jitter on
;;; each axis plus a hue. Indexed by point number, so the cloud is
;;; identical every run with no state and no ordering.
(define bx  (make-vector N 0.0))
(define by  (make-vector N 0.0))
(define bz  (make-vector N 0.0))
(define hue (make-vector N 0.0))

(let fill ((i 0))
  (if (< i N)
      (let ((r  (threefry4x32-unit (vector i 0 0 0) key))
            (ix (modulo i SIDE))
            (iy (modulo (quotient i SIDE) SIDE))
            (iz (quotient i (* SIDE SIDE))))
        (vector-set! bx i (+ (grid-coord ix) (* 0.05 (- (vector-ref r 0) 0.5))))
        (vector-set! by i (+ (grid-coord iy) (* 0.05 (- (vector-ref r 1) 0.5))))
        (vector-set! bz i (+ (grid-coord iz) (* 0.05 (- (vector-ref r 2) 0.5))))
        (vector-set! hue i (vector-ref r 3))
        (fill (+ i 1)))))

(define (update! t)
  (orbit-camera! cam)
  (let loop ((i 0))
    (if (< i N)
        (let* ((x0 (vector-ref bx i))
               (y0 (vector-ref by i))
               (z0 (vector-ref bz i))
               (h  (vector-ref hue i))
               ;; A standing wave through the lattice — a placeholder for
               ;; the noise field a compute wrangle will evaluate.
               (w  (sin (+ (* 2.2 x0) (* 1.7 y0) (* 2.9 z0) t)))
               (d  (* 0.16 w))
               (lit (* 0.5 (+ 1.0 w))))
          (point-set! pts i
                      (+ x0 d) (+ y0 d) (+ z0 d)
                      (+ 0.008 (* 0.010 h))
                      (+ 0.30 (* 0.60 lit))
                      (+ 0.35 (* 0.45 h))
                      (+ 0.55 (* 0.40 (- 1.0 lit))))
          (loop (+ i 1))))))

(run-points-loop buf N update! cam "vxs-gpu-canvas")
