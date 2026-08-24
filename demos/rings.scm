;;; ==========================================================
;;; GPU Polar Rings — conditionals in the kernel language
;;; ==========================================================
;;; (if c a b) compiles to WGSL select(b, a, c), which is BRANCHLESS: both
;;; arms are evaluated and neither short-circuits. That is the right
;;; default on a GPU, where a real branch diverges the warp — but it means
;;; an arm can never guard the other from a bad value.
;;;
;;; Hard edges are what conditionals buy. Without them every kernel this
;;; language can express is a smooth gradient.

(load "lib/gpu.scm")

(define-kernel rings
  (let ((c (- uv 0.5))
        (r (length c))
        (band (fract (- (* r 12.0) (* time 0.8))))
        (edge (if (< band 0.5) 1.0 0.0))
        (glow (+ 0.35 (* 0.35 (sin (+ (* r 18.0) time))))))
    (vec3 (* edge glow)
          (* edge (+ 0.25 (* 0.5 (fract (+ r (* time 0.2))))))
          (+ 0.35 (* 0.45 (- 1.0 edge))))))

(run-kernel-loop rings "vxs-gpu-canvas")
