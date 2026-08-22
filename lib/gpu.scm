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
(load "lib/cubes.scm")

;; PAUSING. Every loop below keeps DRAWING while paused and only stops the
;; simulation CLOCK. Stopping the scheduler instead would freeze the
;; renderer too — it is a fiber like anything else — and the camera with
;; it, which is precisely when you most want to orbit. So `paused?` holds
;; the clock still and everything else carries on: the shader sees a frozen
;; time, update! is still called so the camera still responds, and the
;; scene still redraws from whatever angle you drag it to.

;; (run-kernel-loop wgsl [canvas-id]) -> future
;;
;; Acquires the device and animates the kernel, one frame per yield, off
;; whatever driver is pumping the scheduler.
;;
;; COMPILE FIRST, THEN LOOP. Every loop below touches (gpu-compile device
;; src) once, up front, and passes the resulting shader handle to the draw.
;; That ordering is not a convenience: the draw primitives take a handle
;; and nothing else, so a shader physically cannot reach a pipeline without
;; someone having waited for its compile to succeed. A bad shader now stops
;; the fiber at the `touch`, before the first frame, with the line, the
;; column and the offending source — instead of running at sixty frames a
;; second against a black canvas.
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
             (device  (touch (request-device adapter)))
             (shader  (touch (gpu-compile device wgsl))))
        (let loop ((t 0.0) (last (/ (current-time) 1000.0)))
          (let* ((now (/ (current-time) 1000.0))
                 (t2 (if (paused?) t (+ t (- now last)))))
            (gpu-run-kernel! device shader t2 canvas)
            (yield)
            (loop t2 now)))))))

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
             (device  (touch (request-device adapter)))
             (shader  (touch (gpu-compile device points-wgsl))))
        (let loop ((t 0.0) (last (/ (current-time) 1000.0)))
          (let* ((now (/ (current-time) 1000.0))
                 (t2 (if (paused?) t (+ t (- now last)))))
            ;; update! runs even when paused, because that is where the
            ;; camera is orbited.
            (update! t2)
            (gpu-draw-instances! device shader bytes count t2 camera canvas)
            (yield)
            (loop t2 now)))))))

;; (make-orbiter) -> (lambda (camera) ...) — drag to spin, scroll to zoom.
;;
;; A FACTORY rather than one shared procedure, because the last-position
;; and last-wheel state has to belong to the orbiter. A single shared
;; closure appears to work with one camera and silently breaks with two:
;; the first call consumes the wheel delta and the second sees zero, so the
;; second camera never zooms and nothing says why. orbit-camera! below is
;; just the instance most programs want.
;;
;; Reads the page's mouse primitives directly rather than taking events,
;; because a fiber polls: there is nowhere to deliver an event TO. Keeps its
;; own last-position and last-wheel state in a closure, and on the first
;; call after a gap only RECORDS them — otherwise the first frame of a drag
;; would jump by however far the pointer had travelled since last time, and
;; the first frame after loading would zoom by however much the page had
;; been scrolled before the program started.
;;
;; Zoom is multiplicative. Additive zoom crawls when you are far away and
;; slams into the origin when you are close; scaling by exp(k * scroll)
;; makes a notch of the wheel cover the same PROPORTION of the distance
;; wherever you happen to be.
(define (make-orbiter)
  (let ((last-x 0.0) (last-y 0.0) (last-wheel 0.0)
        (dragging #f) (started #f))
    (lambda (camera)
      (let ((x (mouse-x)) (y (mouse-y)) (wheel (mouse-wheel)))
        (if (not started)
            (begin (set! last-wheel wheel) (set! started #t)))
        ;; --- drag to orbit ---
        (if (mouse-down?)
            (begin
              (if dragging
                  (begin
                    (camera-yaw-set! camera
                                     (+ (camera-yaw camera)
                                        (* 0.01 (- x last-x))))
                    ;; Pitch is clamped just shy of straight up or down:
                    ;; at exactly +/- pi/2 the yaw axis and the view axis
                    ;; line up and the orbit loses a degree of freedom.
                    (camera-pitch-set! camera
                                       (max -1.5
                                            (min 1.5
                                                 (+ (camera-pitch camera)
                                                    (* 0.01 (- y last-y))))))))
              (set! dragging #t))
            (set! dragging #f))
        ;; --- scroll to zoom ---
        (let ((dw (- wheel last-wheel)))
          (if (not (= dw 0.0))
              (camera-distance-set!
               camera
               (max 0.35
                    (min 24.0
                         (* (camera-distance camera) (exp (* 0.0014 dw))))))))
        (set! last-x x)
        (set! last-y y)
        (set! last-wheel wheel)))))

;; The orbiter most programs want. Two viewports want two orbiters.
(define orbit-camera! (make-orbiter))

;; (run-wrangle-loop seed-bytes count wrangle-src frame! camera
;;                    [canvas-id [params]])
;;
;; The GPU-resident counterpart of run-points-loop. The point data lives in
;; a GPU buffer, a compute dispatch rewrites it, and the draw reads it — the
;; host touches none of it per frame. `seed-bytes` seeds the buffer once at
;; the start, which is the only upload that happens.
;;
;; `frame!` is called with the time before each dispatch and exists for
;; whatever the host still owns — orbiting the camera, mostly. It is not
;; where point work belongs any more.
;;
;; The optional `params` is a block from make-wrangle-params. Write it in
;; frame! and the kernel sees the new values on the very next dispatch,
;; with no recompile: that is what makes a slider playable rather than
;; merely demonstrable. Pass #f (or omit it) and the slots read zero.
(define (run-wrangle-loop seed-bytes count wrangle-src frame! camera . opts)
  (let ((canvas (if (null? opts) "gpu-canvas" (car opts)))
        (params (if (or (null? opts) (null? (cdr opts))) #f (cadr opts))))
    (future
      (let* ((adapter (touch (request-adapter)))
             (device  (touch (request-device adapter)))
             ;; Both shaders compile before the buffer is even uploaded, so
             ;; a typo in either one costs a message and no frames.
             (kernel  (touch (gpu-compile device wrangle-src)))
             (draw    (touch (gpu-compile device points-wgsl)))
             (buf     (gpu-buffer device seed-bytes)))
        (display "wrangle: shaders compiled, buffer uploaded, dispatching.") (newline)
        (let loop ((t 0.0) (last (/ (current-time) 1000.0)))
          (let* ((now (/ (current-time) 1000.0))
                 (t2 (if (paused?) t (+ t (- now last)))))
            (frame! t2)
            ;; The dispatch still runs while paused. It is a pure function
            ;; of (index, time), so re-running it with a frozen clock
            ;; reproduces the identical cloud — the counter-based RNG is
            ;; what makes a paused frame stable rather than shimmering.
            (gpu-wrangle! device buf kernel count t2 1 params)
            (gpu-draw-buffer! device buf draw count t2 camera canvas)
            (yield)
            (loop t2 now)))))))

;; (run-cubes-loop bytes count update! camera [canvas-id]) -> future
;;
;; map cube over the point buffer: the same seven floats per point, drawn
;; as solid geometry rather than sprites. The buffer is uploaded per frame
;; exactly as run-points-loop does, so anything that fills one can fill the
;; other and the only change at the call site is which loop is running.
(define (run-cubes-loop bytes count update! camera . opts)
  (let ((canvas (if (null? opts) "gpu-canvas" (car opts))))
    (future
      (let* ((adapter (touch (request-adapter)))
             (device  (touch (request-device adapter)))
             (shader  (touch (gpu-compile device cubes-wgsl)))
             (buf     (gpu-buffer device bytes)))
        (let loop ((t 0.0) (last (/ (current-time) 1000.0)))
          (let* ((now (/ (current-time) 1000.0))
                 (t2 (if (paused?) t (+ t (- now last)))))
            (update! t2)
            ;; The buffer lives on the GPU, so a host-side writer has to
            ;; push its changes each frame. gpu-buffer-write! is that push.
            (gpu-buffer-write! device buf bytes)
            (gpu-draw-geometry! device buf shader cube-vertex-count
                                count t2 camera canvas)
            (yield)
            (loop t2 now)))))))
