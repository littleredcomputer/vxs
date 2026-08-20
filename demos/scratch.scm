;;; ==========================================================
;;; Scratch — edit this file in your own editor, save, watch it run
;;; ==========================================================
;;; Watch mode fetches this file over the static server, along with every
;;; lib/*.scm, and re-runs on save. The libraries are re-registered each
;;; pass, so editing lib/points.scm or lib/wgsl.scm takes effect here
;;; WITHOUT a rebuild — the browser normally reads copies baked into the
;;; wasm binary at build time.
;;;
;;; Serve from the repo root, not from web/:
;;;   python3 -m http.server
;;; then open  http://localhost:8000/web/index.html

(load "lib/gpu.scm")
(load "lib/threefry.scm")

(define N 1200)
(define buf (make-points N))
(define pts (points-view buf))
(define key (vector 0 0 0 0))

(define radius (make-vector N 0.0))
(define phase  (make-vector N 0.0))
(define speed  (make-vector N 0.0))
(define hue    (make-vector N 0.0))

(let fill ((i 0))
  (if (< i N)
      (let ((r (threefry4x32-unit (vector i 0 0 0) key)))
        (vector-set! radius i (* 0.85 (sqrt (vector-ref r 0))))
        (vector-set! phase  i (* 6.2831 (vector-ref r 1)))
        (vector-set! speed  i (+ 0.15 (* 0.9 (vector-ref r 2))))
        (vector-set! hue    i (vector-ref r 3))
        (fill (+ i 1)))))

(define (update! t)
  (let loop ((i 0))
    (if (< i N)
        (let* ((r (vector-ref radius i))
               (a (+ (vector-ref phase i) (* t (vector-ref speed i))))
               (h (vector-ref hue i)))
          (point-set! pts i
                      (* r (cos a))
                      (* r (sin a))
                      (+ 0.006 (* 0.012 h))
                      (+ 0.25 (* 0.75 h))
                      (+ 0.35 (* 0.45 (sin (+ a t))))
                      (+ 0.55 (* 0.45 (cos (* 0.7 t)))))
          (loop (+ i 1))))))

(run-points-loop buf N update! "vxs-gpu-canvas")
