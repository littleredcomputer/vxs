;;; ==========================================================
;;; A programmable surface: one curve, three jobs
;;; ==========================================================
;;; `curve-elem` below is a define-dual — one body, compiled for the GPU
;;; and run by the VM. It is the model's likelihood, the inference's
;;; arithmetic, and the picture's geometry. There is no second copy of it
;;; anywhere, so changing it changes all three.
;;;
;;; Things to try, in rising order of how much they move:
;;;
;;;   the prior on `a` down to 0.2   starve the family of bendiness, and
;;;                                  watch the posterior go straight
;;;   the basis to (sin (* b x))     a different family entirely; the
;;;                                  frame follows it without being told
;;;   add a fourth parameter         give curve-elem a `d` and the model
;;;                                  an (at :d ...) — the plot picks it up
;;;                                  from the model's own choices
;;;
;;; The picture is not N curves blended. Per pixel the kernel loops the
;;; particles and accumulates coverage, so the image IS the posterior
;;; predictive density — and the stroke is a uniform width because the
;;; distance is normalised by its screen-space gradient, which measures
;;; in pixels rather than in plot units.

;; gpu.scm explicitly, though curveplot.scm pulls it in anyway: loading it
;; re-runs lib/wrangle.scm, which puts the global scratch and shared
;; declarations back to a known state. A demo that skipped it would
;; inherit whatever the previously clicked demo declared.
(load "lib/gpu.scm")
(load "lib/curveplot.scm")

;;--- the curve ----------------------------------------------------------

(define-dual (curve-elem (x :f32) (a :f32) (b :f32) (c :f32))
  (+ (* a x x) (* b x) c))

;;--- the model ----------------------------------------------------------

(define-gen (curve xs sigma npts)
  (let* ((a (at :a (normal 0 1.5)))
         (b (at :b (normal 0 1.5)))
         (c (at :c (normal 0 1.5))))
    (at :ys (batch-i npts (j)
              (normal (curve-elem (view-ref xs j) a b c) sigma)))))

;;--- knobs --------------------------------------------------------------

(define NPTS  10)     (define NOISE 0.5)    ; the data
(define SIGMA 1.0)                          ; the noise the MODEL assumes
(define K     4000)   (define NDRAW 384)    ; particles, and how many are drawn
(define SEED  1)
(define TRUTH '(2.0 -1.0 0.5))              ; never shown to the model

;;--- the data -----------------------------------------------------------

(define xs (bytes-view (make-bytes (* NPTS 8)) :f64))
(let loop ((i 0))
  (if (< i NPTS)
      (begin (view-set! xs i (+ -2.0 (* (/ 4.0 NPTS) i))) (loop (+ i 1)))))

;; Generated with the same curve the model will use, so the observations
;; follow the family too — swap the basis and the data swaps with it.
(define ys (bytes-view (make-bytes (* NPTS 8)) :f64))
(let loop ((i 0))
  (if (< i NPTS)
      (begin (view-set! ys i (apply curve-elem (cons (view-ref xs i) TRUTH)))
             (loop (+ i 1)))))
(rng-fill-normal! (rng-make 0 9 0) ys 0 NPTS ys NOISE)

;;--- the fit ------------------------------------------------------------
;; Importance sampling on the host, which at this K is a few tens of
;; milliseconds. Only the PICTURE is on the device; the claim being made
;; here is about one definition serving three consumers, and it is equally
;; true with the scoring up here.

(define cols (let ((was (eval-budget-ms! 8000)))
               (unwind-protect (importance (curve xs SIGMA NPTS) {:ys ys} K SEED)
                               (eval-budget-ms! was))))

(define ws (:weights cols))
(normalize-weights! ws K)

(define ess
  (let loop ((i 0) (s 0.0))
    (if (= i K) (/ 1.0 s) (loop (+ i 1) (+ s (* (view-ref ws i) (view-ref ws i)))))))

;; Resampled by weight, duplicates included: a duplicate is what makes a
;; concentrated posterior LOOK concentrated.
(define picks (bytes-view (make-bytes (* NDRAW 4)) :i32))
(rng-fill-categorical! (rng-make 0 SEED 0) picks 0 NDRAW ws)

(display (format "ESS ~a of ~a\n" (inexact->exact (round ess)) K))

;;--- the picture --------------------------------------------------------
;; Told the model and the curve's NAME, and nothing about the shape of
;; either. The parameter list comes from the model's own choices.

(plot-posterior! (curve xs SIGMA NPTS) 'curve-elem cols picks TRUTH ys)
