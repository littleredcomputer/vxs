;;; Painting for a curve fit — deliberately quarantined.
;;;
;;; None of this is about probability. It is here so that the demo file
;;; next to it can be knobs, a model, and a call, with nothing between the
;;; reader and the four lines that matter.
;;;
;;; It lives in lib/ rather than demos/ for one unglamorous reason: every
;;; lib/*.scm is compiled into the binary (see src/Makefile), and the
;;; browser has no filesystem. `load` resolves an embedded copy by
;;; BASENAME, so (load "lib/fitplot.scm") means the same thing in the page
;;; as it does on disk. A demos/ path would work natively and 404 in the
;;; browser, which is the worst of the three possible outcomes.

;;--- the window ---------------------------------------------------------
;; FIXED, not fitted to the data. Autoscaling is the obvious thing and it
;; destroys the only comparison this picture exists to make: it would
;; shrink the prior's fan until it looked exactly as tidy as the
;; posterior, and the two would appear equally certain.
;;
;; The cost is that a badly tuned model draws off the top. That is the
;; correct failure — a curve that leaves the frame IS the news.

(define plot-x0 -2.2)
(define plot-x1  2.2)
(define plot-y0 -5.0)
(define plot-y1 14.0)
(define plot-segs 24)

(define (plot-window! x0 x1 y0 y1)
  (set! plot-x0 x0) (set! plot-x1 x1)
  (set! plot-y0 y0) (set! plot-y1 y1))

(define (plot-2dp x) (/ (round (* 100.0 x)) 100.0))

;; The posterior mean under weights that are ALREADY NORMALISED — linear
;; probabilities summing to one, not log-weights. lib/gen.scm's
;; weighted-mean does its own logsumexp and expects the log form, so
;; calling it here would quietly exponentiate twice. Everything in this
;; file runs after normalize-weights!, which is also what rng-categorical!
;; needs, so there is one convention in play and this is it.
(define (plot-mean col ws K)
  (let loop ((i 0) (m 0.0))
    (if (= i K) m (loop (+ i 1) (+ m (* (view-ref ws i) (view-ref col i)))))))

(define (plot-px x) (* (canvas-width)  (/ (- x plot-x0) (- plot-x1 plot-x0))))
(define (plot-py y) (* (canvas-height) (- 1.0 (/ (- y plot-y0) (- plot-y1 plot-y0)))))

;;--- pieces -------------------------------------------------------------

(define (plot-curve! a b c r g bl al)
  (let loop ((s 0) (lx 0.0) (ly 0.0))
    (if (<= s plot-segs)
        (let* ((x (+ plot-x0 (* (/ (- plot-x1 plot-x0) plot-segs) s)))
               (cx (plot-px x))
               (cy (plot-py (+ (* a x x) (* b x) c))))
          (if (> s 0) (canvas-draw-line lx ly cx cy r g bl al))
          (loop (+ s 1) cx cy)))))

(define (plot-grid!)
  (let loop ((y -4))
    (if (<= y 12)
        (begin (canvas-draw-line (plot-px plot-x0) (plot-py y)
                                 (plot-px plot-x1) (plot-py y)
                                 0.28 0.34 0.44 0.30)
               (loop (+ y 4)))))
  (canvas-draw-line (plot-px 0.0) (plot-py plot-y0)
                    (plot-px 0.0) (plot-py plot-y1)
                    0.34 0.40 0.50 0.40))

;; Draw one curve per particle named by `picks`, an :i32 view of indices.
(define (plot-curves-at! soa picks r g bl al)
  (let ((as (:a soa)) (bs (:b soa)) (cs (:c soa)))
    (let loop ((m 0))
      (if (< m (view-length picks))
          (let ((j (view-ref picks m)))
            (plot-curve! (view-ref as j) (view-ref bs j) (view-ref cs j) r g bl al)
            (loop (+ m 1)))))))

;; Draw the FIRST n particles, unweighted. No index buffer, because the
;; indices would be 0..n-1 and building them to look them up again is a
;; detour with nothing at the end of it.
(define (plot-curves-first! soa n r g bl al)
  (let ((as (:a soa)) (bs (:b soa)) (cs (:c soa)))
    (let loop ((j 0))
      (if (< j n)
          (begin (plot-curve! (view-ref as j) (view-ref bs j) (view-ref cs j) r g bl al)
                 (loop (+ j 1)))))))

;;--- the whole picture --------------------------------------------------
;;
;; (plot-fit! xs ys n soa picks truth ess)
;;   xs ys  : :f64 views of the n observations
;;   soa    : the columnar result of `importance` — :a :b :c
;;   picks  : an :i32 view of particle indices resampled BY WEIGHT, so
;;            duplicates are information: where they pile up is where
;;            belief piles up
;;   truth  : (a b c), or #f to draw no reference curve
;;   ess    : effective sample size, for the caption
;;
;; The weights in `soa` must already be NORMALISED (normalize-weights!).
;;
;; The grey prior fan is drawn from particles 0..len(picks), taken
;; UNWEIGHTED — no second sampler and no second seed, because those
;; particles already are prior draws. That is the whole content of
;; importance sampling in one line of plotting code: the same guesses
;; appear twice, and the only difference between the two sets of curves is
;; whether the weights were consulted.

(define (plot-fit! xs ys n soa picks truth ess)
  (canvas-clear 0.02 0.03 0.05 1.0)
  (plot-grid!)
  (plot-curves-first! soa (view-length picks) 0.46 0.52 0.62 0.16)  ; prior
  (plot-curves-at!    soa picks               0.16 0.86 0.76 0.28)  ; posterior

  (if truth
      (plot-curve! (car truth) (cadr truth) (caddr truth) 0.96 0.62 0.18 0.90))

  (let loop ((i 0))
    (if (< i n)
        (begin (canvas-draw-circle (plot-px (view-ref xs i)) (plot-py (view-ref ys i))
                                   3.6 0.98 0.90 0.70 1.0)
               (loop (+ i 1)))))

  (let ((as (:a soa)) (bs (:b soa)) (ws (:weights soa)) (cs (:c soa))
        (K (:n soa)))
    (canvas-draw-text
     (format "~a points   K ~a   ESS ~a" n K (inexact->exact (round ess)))
     16.0 26.0 0.84 0.89 0.96 1.0)
    (canvas-draw-text
     (format "posterior mean   a ~a   b ~a   c ~a"
             (plot-2dp (plot-mean as ws K))
             (plot-2dp (plot-mean bs ws K))
             (plot-2dp (plot-mean cs ws K)))
     16.0 46.0 0.16 0.86 0.76 1.0)
    (if truth
        (canvas-draw-text
         (format "truth            a ~a   b ~a   c ~a"
                 (plot-2dp (car truth)) (plot-2dp (cadr truth)) (plot-2dp (caddr truth)))
         16.0 66.0 0.96 0.62 0.18 1.0))))
