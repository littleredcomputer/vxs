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
(load "lib/wrangle.scm")

;; (run-kernel-loop wgsl [canvas-id]) -> future
;;
;; Acquires the device and animates the kernel, one frame per yield, off
;; whatever driver is pumping the scheduler.
;;
;; NOTHING HERE CATCHES. These loops used to wrap the draw in a guard that
;; printed a message and stopped — and it was worse than useless, because
;; `error-object-message` returns the error's TAG, not its text. A shader
;; that failed to compile reported the single word "wgsl" and discarded the
;; compile log underneath it.
;;
;; An uncaught error kills the fiber, which stops the loop just the same,
;; and the scheduler now reports the death IN FULL to the page terminal. So
;; removing the guard costs nothing and recovers the message. At this stage
;; a failure that blasts through with everything attached beats a tidy one
;; that has thrown the evidence away.
;;
;; The rule that still matters: the awaits sit outside any guard anyone
;; might add later, because guard cannot suspend a fiber and `touch` does.
(define (run-kernel-loop wgsl . opts)
  (let ((canvas (if (null? opts) "gpu-canvas" (car opts))))
    (future
      (let* ((adapter (touch (request-adapter)))
             (device  (touch (request-device adapter))))
        (let loop ()
          (gpu-run-kernel! device wgsl (/ (current-time) 1000.0) canvas)
          (yield)
          (loop))))))

;; (run-points-loop bytes count update! camera [canvas-id]) -> future
;;
;; The instanced-points counterpart of run-kernel-loop. `update!` is called
;; with the time in seconds before each draw and is expected to write the
;; point buffer; passing a procedure that does nothing gives a static
;; cloud. `camera` is a mutable 4-vector (see make-camera) — update! can
;; orbit it in place, which is why it is a vector and not four arguments.
;;
;; A mistake in update! kills the fiber, and the scheduler reports the death
;; in full to the page terminal. That is on purpose — see the note at the
;; top of this file about why nothing here catches.
(define (run-points-loop bytes count update! camera . opts)
  (let ((canvas (if (null? opts) "gpu-canvas" (car opts))))
    (future
      (let* ((adapter (touch (request-adapter)))
             (device  (touch (request-device adapter))))
        (let loop ()
          (let ((t (/ (current-time) 1000.0)))
            (update! t)
            (gpu-draw-instances! device points-wgsl bytes count t camera canvas)
            (yield)
            (loop)))))))

;; (orbit-camera! camera) — drag to spin, called from inside update!.
;;
;; Reads the page's mouse primitives directly rather than taking events,
;; because a fiber polls: there is nowhere to deliver an event TO. Keeps
;; its own last-position state in a closure so the first frame of a drag
;; does not jump.
(define orbit-camera!
  (let ((last-x 0.0) (last-y 0.0) (dragging #f))
    (lambda (camera)
      (let ((x (mouse-x)) (y (mouse-y)))
        (if (mouse-down?)
            (begin
              (if dragging
                  (begin
                    (camera-yaw-set! camera
                                     (+ (camera-yaw camera)
                                        (* 0.01 (- x last-x))))
                    (camera-pitch-set! camera
                                       (max -1.5
                                            (min 1.5
                                                 (+ (camera-pitch camera)
                                                    (* 0.01 (- y last-y))))))))
              (set! dragging #t))
            (set! dragging #f))
        (set! last-x x)
        (set! last-y y)))))

;; (run-wrangle-loop device-less: buf count wrangle-src update-camera! camera [canvas])
;;
;; The GPU-resident counterpart of run-points-loop. The point data lives in
;; a GPU buffer, a compute dispatch rewrites it, and the draw reads it — the
;; host touches none of it per frame. `seed-bytes` seeds the buffer once at
;; the start, which is the only upload that happens.
;;
;; `frame!` is called with the time before each dispatch and exists for
;; whatever the host still owns — orbiting the camera, mostly. It is not
;; where point work belongs any more.
(define (run-wrangle-loop seed-bytes count wrangle-src frame! camera . opts)
  (let ((canvas (if (null? opts) "gpu-canvas" (car opts))))
    (future
      (let* ((adapter (touch (request-adapter)))
             (device  (touch (request-device adapter)))
             (buf     (gpu-buffer device seed-bytes)))
        (display "wrangle: buffer uploaded, dispatching.") (newline)
        (let loop ()
          (let ((t (/ (current-time) 1000.0)))
            (frame! t)
            (gpu-wrangle! device buf wrangle-src count t 1)
            (gpu-draw-buffer! device buf points-wgsl count t camera canvas)
            (yield)
            (loop)))))))
