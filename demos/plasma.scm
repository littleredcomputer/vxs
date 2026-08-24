;;; ==========================================================
;;; GPU Plasma Field — WGSL compiled from Scheme, in the browser
;;; ==========================================================
;;; Nothing here writes WGSL. The kernel below is compiled to a shader by
;;; lib/wgsl.scm, which type-checks it first: mixing a vec2 with a vec3,
;;; or swizzling past the end of a vector, is a Scheme error here rather
;;; than a shader compilation log in the browser console.
;;;
;;; There is no quote on the kernel body. define-kernel is a macro, so it
;;; receives the form unevaluated — the language boundary is the syntax
;;; rather than a punctuation mark. Inside it you are writing kernel code,
;;; where uv, time and res are the inputs and the result is a vec3 colour.
;;;
;;; lib/*.scm is embedded in the wasm binary, so load works here even
;;; though the browser has no filesystem.

(load "lib/gpu.scm")

(define-kernel plasma
  (let ((c (- uv 0.5))
        (r (length c))
        (a (* 6.2831 (+ (* r 3.0) (* time 0.15)))))
    (vec3 (+ 0.5 (* 0.5 (sin (- (* r 24.0) (* time 3.0)))))
          (+ 0.5 (* 0.5 (cos (+ a (* time 0.7)))))
          (+ 0.6 (* 0.4 (sin (* time 1.3)))))))

;;; The device arrives as an ordinary future; the loop runs one frame per
;;; yield, pumped by the same driver the CPU demos use. All of that lives
;;; in run-kernel-loop, including the rule that guard must wrap only the
;;; draw and never the yield — guard cannot suspend a fiber.
(run-kernel-loop plasma "vxs-gpu-canvas")
