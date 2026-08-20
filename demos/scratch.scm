;;; ==========================================================
;;; Scratch — a cube of points in 3D. Drag to orbit, scroll to zoom.
;;; ==========================================================
;;; Watch mode fetches this file over the static server, along with every
;;; lib/*.scm, and re-runs on save. The libraries are re-registered each
;;; pass, so editing lib/points.scm or lib/wgsl.scm takes effect here
;;; WITHOUT a rebuild.
;;;
;;; Serve from the repo root, not from web/:
;;;   python3 -m http.server
;;; then open  http://localhost:8000/web/index.html
;;;
;;; This is the CPU reference: Scheme writes every point, every frame. The
;;; wrangle preset does the same job on the GPU. Watch the scene rate in
;;; the corner — the browser paints at 60 FPS regardless, but this pass
;;; costs more than the scheduler's frame budget, so the scene advances
;;; more slowly than the page repaints. That gap is what the wrangle
;;; closes.

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
