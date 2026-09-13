;;; ==========================================================
;;; The f32 measurement: what the device computes, against the VM
;;; ==========================================================
;;; `stage` produces one description of a log-joint and two backends
;;; consume it — `staged-logpdf` evaluates it on the VM in f64, and
;;; `staged-kernel` emits it for the device, which computes in f32. Until
;;; now the kernel's TEXT was checked and the kernel was never run. This
;;; runs it.
;;;
;;; It is a MEASUREMENT, not a test. The two sides compute in different
;;; precisions, so they cannot agree exactly, and a tolerance tightened
;;; until it passes would only be recording the machine it last ran on.
;;; What a number here is good for is knowing the size of the gap before
;;; anything is built on top of it — a rejuvenation kernel accepting or
;;; rejecting a move on a log-ratio inherits this error, and a conjugate
;;; update is a much worse place to discover it.
;;;
;;; WHAT IS ISOLATED. The device can only ever see f32 inputs, so a naive
;;; comparison would measure the upload's rounding along with the
;;; arithmetic's. Every input here is written into an f32 buffer FIRST and
;;; read back out of it, and the VM is then given those exact values. The
;;; two sides therefore differ in one respect only: the width of the
;;; arithmetic. Both see bit-identical inputs.
;;;
;;; The particles come from `importance` rather than from a grid, because
;;; the regime that matters is the one inference actually visits. A sweep
;;; over implausible parameters would measure the wrong part of the range.

;; lib/stage.scm FIRST. It loads lib/wgsl.scm, which resets the terminal
;; table, and lib/wrangle.scm registers `point` into that table when
;; lib/gpu.scm pulls it in. Loading these the other way round costs the
;; `point` terminal and the error names the operator rather than the
;; cause, so the order is worth stating rather than discovering twice.
(load "lib/stage.scm")
(load "lib/gpu.scm")

(define NPTS 32)
;; The host does K importance samples and K oracle evaluations before the
;; dispatch, and the preset harness caps an evaluation at 750ms. 512 is
;; comfortably inside that under wasm and still finds a worst case.
(define K 512)
(define SIGMA 1.0)

;;--- the model ----------------------------------------------------------
;; The same curve basis.scm fits, and the same define-dual: one body, run
;; by the VM for the oracle and compiled for the device for the subject.
;; There is no second copy to drift.

(define-dual (curve-elem (x :f32) (a :f32) (b :f32) (c :f32))
  (+ (* a x x) (* b x) c))

(define-gen (curve xs sigma npts)
  (let* ((a (at :a (normal 0 1.5)))
         (b (at :b (normal 0 1.5)))
         (c (at :c (normal 0 1.5))))
    (at :ys (batch-i npts (j)
              (normal (curve-elem (view-ref xs j) a b c) sigma)))))

;;--- the data -----------------------------------------------------------
;; f32-backed views, so what the VM reads is already what the device will
;; read. The data is deterministic and its provenance does not matter: the
;; measurement is of arithmetic, not of inference, and a fixed dataset
;; makes the number reproducible between runs.

(define (f32-view n)
  (let ((b (make-bytes (* n 4))))
    (bytes-seal! b)
    (bytes-view b :f32)))

(define xs (f32-view NPTS))
(define ys (f32-view NPTS))

(let loop ((j 0))
  (if (< j NPTS)
      (let ((x (+ -2.0 (* 4.0 (/ (exact->inexact j) (- NPTS 1))))))
        (view-set! xs j x)
        ;; A quadratic the model can represent, plus a wobble it cannot,
        ;; so the residuals are not all near zero and the likelihood terms
        ;; carry real magnitude.
        (view-set! ys j (+ (* 0.6 x x) (* -0.4 x) 0.25 (* 0.3 (sin (* 3.0 x)))))
        (loop (+ j 1)))))

;;--- staging ------------------------------------------------------------

(define st (stage (curve xs SIGMA NPTS)))

;;--- the device interface -----------------------------------------------
;; The staged kernel's free names ARE the interface: a scalar choice is a
;; plain name, and a batched choice or a data buffer is a one-argument
;; accessor of the same name. Scratch attributes supply the first —
;; an attribute name in a kernel means this point's value, which is
;; exactly what a per-particle scalar choice is.

(scratch-attributes! '((a :f32) (b :f32) (c :f32) (lj :f32) (ljb :f32)))
(shared-layout! (list (list 'xs NPTS) (list 'ys NPTS)))

;; shared-layout! generates (shared-xs j); the staged kernel says (xs j).
;; Two lines of shim and the names meet. `xs` as a kernel function and
;; `xs` as a host view are the same data in two worlds — define-gpu emits
;; no VM procedure, so the Scheme binding above is untouched.
(define-gpu (xs (j :u32)) (shared-xs j))
(define-gpu (ys (j :u32)) (shared-ys j))

;; Fields are total, attributes are partial: naming position, pscale and
;; colour says "unchanged", and the score is the only thing each kernel
;; writes. Nothing is drawn — the points buffer exists because a wrangle
;; dispatch takes one, not because there is a picture.
;;
;; TWO SHADERS RATHER THAN ONE, and not by preference. Putting both folds
;; in one body emits `var acc_2` twice at function scope, because
;; wgsl-compile resets the name counter per sub-expression — deliberately,
;; so emitted text is comparable by string in the tests — and
;; wrangle-point-terminal compiles each attribute argument separately. The
;; two folds therefore land on the same accumulator name and the scalar one
;; reads the blocked vec4. Two dispatches into two different attributes of
;; the same scratch avoid it: one fold per kernel, one readback for both.
(define wsrc
  (wrangle-scheme `(point position pscale colour (lj ,(staged-kernel st)))))

;; The second form is the one a compiler has no licence to remove: four
;; partial sums in a rotating vec4, a reassociation rather than a
;; compensation. Compensated summation was tried here and the compiler
;; deleted it — bit-identical results for all 512 particles — so it is
;; gone and this is what remains to be measured.
(define wsrcb
  (wrangle-scheme `(point position pscale colour
                          (ljb ,(with-staged-blocked (staged-kernel st))))))

;;--- the particles ------------------------------------------------------

(define imp (importance (curve xs SIGMA NPTS) {:ys ys} K 20260912))

(define PTS (make-points K))
(define SB (make-scratch K))
(define SV (scratch-view SB))
(define SH (make-shared))
(define SHV (shared-view SH))

(let loop ((j 0))
  (if (< j NPTS)
      (begin (shared-set! SHV 'xs j (view-ref xs j))
             (shared-set! SHV 'ys j (view-ref ys j))
             (loop (+ j 1)))))

;; Write each particle's parameters into the f32 scratch, then read them
;; back. The values that come back are what the device will see, and they
;; are what the oracle is evaluated at.
;;
;; The oracle is the f64 evaluation of the same IR — the more precise
;; side, and so the one that says whether the device is right. lib/stage.scm
;; also has `with-staged-f32`, which narrows the VM to the device's width;
;; it is not used here, because it was measured against a real device and
;; mispredicted it by nearly 2x. The device's arithmetic is the only
;; reliable statement about the device's arithmetic.
(define oracle (make-vector K 0.0))

;; Called from inside the fiber, not at load time: K oracle evaluations is
;; real arithmetic and a top-level evaluation is capped at 750ms, which a
;; fiber is not.
(define (fill-oracle!)
  (let loop ((i 0))
    (if (< i K)
        (begin
          (scratch-set! SV i 'a (view-ref (:a imp) i))
          (scratch-set! SV i 'b (view-ref (:b imp) i))
          (scratch-set! SV i 'c (view-ref (:c imp) i))
          (vector-set! oracle i
                       (staged-logpdf st {:a (scratch-ref SV i 'a)
                                          :b (scratch-ref SV i 'b)
                                          :c (scratch-ref SV i 'c)
                                          :ys ys}))
          (loop (+ i 1))))))

;;--- the run ------------------------------------------------------------

;; One f32 step at a magnitude. A deviation of 1e-4 means nothing until
;; you know what the last bit is worth where the answer lives, and these
;; log-joints range over a factor of seven in magnitude — so absolute
;; deviations are not comparable between particles and ULP are.
(define (f32-ulp x)
  (if (= x 0.0)
      0.0
      (expt 2.0 (- (floor (/ (log (abs x)) (log 2.0))) 23.0))))

(define (report label back attr orc)
  (let loop ((i 0) (worst 0.0) (worst-i 0) (total 0.0) (signed 0.0) (mag 0.0))
    (if (= i K)
        (let* ((kf      (exact->inexact K))
               (mean    (/ total kf))
               (bias    (/ signed kf))
               (avg-mag (/ mag kf))
               (u-mean  (f32-ulp avg-mag))
               (u-worst (f32-ulp (vector-ref orc worst-i))))
          (display "── ") (display label)
          (display " ───────────────────────") (newline)
          (display "  log-joint magnitude  ~") (display avg-mag) (newline)
          (display "  mean |d|    ") (display mean)
          (display "   = ") (display (/ mean u-mean)) (display " ulp") (newline)
          (display "  bias        ") (display bias)
          (display "   = ") (display (/ bias u-mean)) (display " ulp") (newline)
          (display "  max |d|     ") (display worst)
          (display "   = ") (display (/ worst u-worst)) (display " ulp")
          (display "  at particle ") (display worst-i) (newline)
          (display "  worst rel   ") (display (/ worst (abs (vector-ref orc worst-i))))
          (newline)
          (display "  |bias|/mean ") (display (/ (abs bias) mean))
          (display "   (near 1 is systematic, near 0 is rounding)") (newline))
        (let* ((d  (- (scratch-ref back i attr) (vector-ref orc i)))
               (ad (abs d)))
          (loop (+ i 1)
                (if (> ad worst) ad worst)
                (if (> ad worst) i worst-i)
                (+ total ad)
                (+ signed d)
                (+ mag (abs (vector-ref orc i))))))))

(display "measure: ") (display K) (display " particles, ")
(display NPTS) (display " observations, ")
(display (+ 3 (* 1 NPTS))) (display " score terms each.") (newline)

(future
  (let* ((adapter (touch (request-adapter)))
         (device  (touch (request-device adapter)))
         (kernel  (touch (gpu-compile device wsrc)))
         (kernelb (touch (gpu-compile device wsrcb))))
    ;; The scratch must hold the particles BEFORE it is uploaded, so the
    ;; oracles are filled here — between the compile and the buffers.
    (fill-oracle!)
    (let* ((buf     (gpu-buffer device PTS))
           (scratch (gpu-buffer device SB))
           (shared  (gpu-buffer device SH)))
      (display "measure: shaders compiled, buffers uploaded, dispatching.")
      (newline)
      ;; Both dispatches read the same particles out of the same scratch
      ;; and write different attributes of it, so one readback carries
      ;; both answers and the comparison is on identical inputs.
      (gpu-wrangle! device buf kernel  K 0.0 1 #f 1 scratch shared)
      (gpu-wrangle! device buf kernelb K 0.0 1 #f 1 scratch shared)
      (let* ((r    (touch (gpu-buffer-read device scratch)))
             (back (scratch-view (cdr r))))
        (report "plain   sum vs vm f64" back 'lj  oracle)
        (report "blocked sum vs vm f64" back 'ljb oracle)
        (display "  Same terms, same truth, two summation orders. Blocked") (newline)
        (display "  should read sub-ulp with the bias gone; plain carries a") (newline)
        (display "  systematic ~2 ulp because a growing accumulator drops") (newline)
        (display "  each addend's low bits, once per term.") (newline)))))
