;;----------------------------------------------------------------------
;; The browser driver for kernel shaders
;;
;; lib/wgsl.scm compiles a kernel; lib/shadertoy.scm wraps it in a
;; fullscreen shader; this file runs the result. It is the only one of the
;; three that needs a host — request-adapter and gpu-run-kernel! exist in
;; the wasm build and nowhere else — which is why it is a separate file and
;; not part of the prelude. The prelude is host-agnostic and every VM pays
;; for it; this is browser-only, so it lives in lib/, which the browser can
;; load because lib/*.scm is embedded in the binary.
;;
;; Loading this file natively is fine. Calling run-kernel-loop natively is
;; not: the GPU primitives are simply unbound there, which surfaces as an
;; ordinary unbound-variable error at the call.
;;----------------------------------------------------------------------

(load "lib/shadertoy.scm")
(load "lib/points.scm")

;; (run-kernel-loop wgsl [canvas-id]) -> future
;;
;; Acquires the device and animates the kernel, one frame per yield, off
;; whatever driver is pumping the scheduler.
;;
;; Three rules are encoded here so that no caller has to remember them:
;;
;;   1. guard wraps ONLY the draw, never the yield. guard cannot suspend a
;;      fiber, so yielding inside one kills it — and silently, which is the
;;      worst version of that bug.
;;   2. The awaits happen OUTSIDE any guard, for the same reason: touch
;;      suspends.
;;   3. A failing draw stops the loop instead of reporting the same error
;;      sixty times a second.
(define (run-kernel-loop wgsl . opts)
  (let ((canvas (if (null? opts) "gpu-canvas" (car opts))))
    (future
      (let* ((adapter (touch (request-adapter)))
             (device  (touch (request-device adapter))))
        (let loop ()
          (if (guard (e (#t (display "kernel failed: ")
                            (display (if (error-object? e)
                                         (error-object-message e)
                                         e))
                            (newline)
                            #f))
                (gpu-run-kernel! device wgsl (/ (current-time) 1000.0) canvas)
                #t)
              (begin (yield) (loop))
              (begin (display "kernel loop stopped.") (newline))))))))

;; (run-points-loop bytes count update! [canvas-id]) -> future
;;
;; The instanced-points counterpart of run-kernel-loop. `update!` is called
;; with the time in seconds before each draw and is expected to write the
;; point buffer; passing a procedure that does nothing gives a static
;; cloud.
;;
;; update! runs INSIDE the guard, so a mistake in it is reported like a
;; failed draw rather than silently killing the fiber — which means it must
;; not yield, for the same reason the draw must not: guard cannot suspend.
(define (run-points-loop bytes count update! . opts)
  (let ((canvas (if (null? opts) "gpu-canvas" (car opts))))
    (future
      (let* ((adapter (touch (request-adapter)))
             (device  (touch (request-device adapter))))
        (let loop ()
          (let ((t (/ (current-time) 1000.0)))
            (if (guard (e (#t (display "points failed: ")
                              (display (if (error-object? e)
                                           (error-object-message e)
                                           e))
                              (newline)
                              #f))
                  (update! t)
                  (gpu-draw-instances! device points-wgsl bytes count t canvas)
                  #t)
                (begin (yield) (loop))
                (begin (display "point loop stopped.") (newline)))))))))
