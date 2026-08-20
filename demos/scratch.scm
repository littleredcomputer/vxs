;;; ==========================================================
;;; Scratch — a cube of points in 3D. Drag the canvas to orbit.
;;; ==========================================================
;;; Watch mode fetches this file over the static server, along with every
;;; lib/*.scm, and re-runs on save. The libraries are re-registered each
;;; pass, so editing lib/points.scm or lib/wgsl.scm takes effect here
;;; WITHOUT a rebuild.
;;;
;;; Serve from the repo root, not from web/:
;;;   python3 -m http.server
;;; then open  http://localhost:8000/web/index.html

(load "lib/gpu.scm")
(load "lib/threefry.scm")

(define SIDE 14)                       ; SIDE^3 points
(define N (* SIDE SIDE SIDE))
(define buf (make-points N))
(define pts (points-view buf))
(define cam (make-camera))
(define key (vector 0 0 0 0))

;;; Lattice position of point i, in -1..1 on each axis.
(define (grid-coord i) (- (* 2.0 (/ i (- SIDE 1.0))) 1.0))

;;; One Threefry block per point gives four independent randoms — jitter
;;; on each axis plus a hue. Indexed by point number, so the cloud is
;;; identical every run with no state and no ordering.
(define jitter (make-vector (* N 4) 0.0))
(let fill ((i 0))
  (if (< i N)
      (let ((r (threefry4x32-unit (vector i 0 0 0) key)))
        (vector-set! jitter (+ (* i 4) 0) (- (vector-ref r 0) 0.5))
        (vector-set! jitter (+ (* i 4) 1) (- (vector-ref r 1) 0.5))
        (vector-set! jitter (+ (* i 4) 2) (- (vector-ref r 2) 0.5))
        (vector-set! jitter (+ (* i 4) 3) (vector-ref r 3))
        (fill (+ i 1)))))

(define (update! t)
  (orbit-camera! cam)
  (let loop ((i 0))
    (if (< i N)
        (let* ((ix (modulo i SIDE))
               (iy (modulo (quotient i SIDE) SIDE))
               (iz (quotient i (* SIDE SIDE)))
               (x0 (grid-coord ix))
               (y0 (grid-coord iy))
               (z0 (grid-coord iz))
               (h  (vector-ref jitter (+ (* i 4) 3)))
               ;; A cheap standing wave through the lattice — a placeholder
               ;; for the noise field a compute wrangle will evaluate.
               (w  (sin (+ (* 2.2 x0) (* 1.7 y0) (* 2.9 z0) t)))
               (d  (* 0.16 w)))
          (point-set! pts i
                      (+ x0 d (* 0.05 (vector-ref jitter (+ (* i 4) 0))))
                      (+ y0 d (* 0.05 (vector-ref jitter (+ (* i 4) 1))))
                      (+ z0 d (* 0.05 (vector-ref jitter (+ (* i 4) 2))))
                      (+ 0.008 (* 0.010 h))
                      (+ 0.30 (* 0.60 (* 0.5 (+ 1.0 w))))
                      (+ 0.35 (* 0.45 h))
                      (+ 0.55 (* 0.40 (- 1.0 (* 0.5 (+ 1.0 w))))))
          (loop (+ i 1))))))

(run-points-loop buf N update! cam "vxs-gpu-canvas")
