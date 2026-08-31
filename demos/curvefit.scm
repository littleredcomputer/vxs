;;; ==========================================================
;;; Curve fit — a probabilistic model, sampled and plotted
;;; ==========================================================
;;; Ten noisy points came off a quadratic. Which quadratic?
;;;
;;; The model below is the whole answer, and there is no inference code
;;; anywhere in this file. `importance` (lib/gen.scm) takes the model and
;;; the data and does the rest, because a model that calls `at` is a
;;; coroutine, and a coroutine can be driven by anything. The painting
;;; lives in lib/fitplot.scm so that it stays out of the way.
;;;
;;; Runs once and stops. Change something, press Cmd-Enter, look again.
;;;
;;; WHAT THE PICTURE SHOWS. Grey curves are the PRIOR: what the model
;;; believed before seeing anything. Teal curves are the POSTERIOR — the
;;; very same particles, resampled by weight — so where they pile up is
;;; where belief piles up. Amber is the truth, which the model is never
;;; told.
;;;
;;; THE NUMBER TO WATCH IS ESS, NOT K. Importance sampling weights K prior
;;; draws by how well each explains the data; the effective sample size is
;;; how many are really doing the work. `ESS 23 of 5000` is not a defect,
;;; it is the honest count. Three things move it:
;;;
;;;   SIGMA down, or NPTS up   ->  sharper posterior, and fewer prior draws
;;;                                land near it. ESS falls, the teal curves
;;;                                collapse to a few distinct lines, and
;;;                                the band stops meaning anything.
;;;   the prior's width down   ->  ESS rises, because the guesses start
;;;                                nearer the answer. Cheating, in exact
;;;                                proportion to how much you narrowed it.
;;;   K up                     ->  ESS rises in proportion. Linear, and the
;;;                                only cure that costs nothing but time.
;;;
;;; Try SIGMA 0.5 and watch the band collapse. Then double K and watch half
;;; of it come back.
;;;
;;; WHY THERE IS A SEED. Particle i is drawn from (rng-make i SEED 0) —
;;; a pure function of its own index — so sample 37 is the same whether you
;;; asked for 40 or 40,000, in any order. That is deliberate in lib/gen.scm
;;; and it is what makes a run reproducible without threading a generator
;;; through every call.
;;;
;;; It has a consequence that surprises: raising K does not redraw
;;; anything. It EXTENDS the same fixed sequence, so the curves you were
;;; already looking at stay exactly where they were and new ones appear
;;; beyond them. Useful — the improvement you see is really the extra
;;; particles and not a luckier shuffle — but it means K is the wrong knob
;;; for asking "was that just a good draw?". SEED is that knob. Change it
;;; for an entirely different set of guesses at the same K.

(load "lib/gen.scm")

;;--- knobs --------------------------------------------------------------

(define NPTS  10)                      ; how many observations
(define NOISE 0.5)                     ; noise actually in the data
(define SIGMA 1.0)                     ; noise the MODEL assumes
(define K     5000)                    ; particles
(define SEED  1)                       ; which guesses. See the note below
(define NDRAW 40)                      ; curves drawn from each distribution

(define A0  2.0)                       ; the truth. Makes the data, and
(define B0 -1.0)                       ; draws the amber line. The model
(define C0  0.5)                       ; is never shown these.

;;--- the data -----------------------------------------------------------

(define xs (bytes-view (make-bytes (* NPTS 8)) :f64))
(let loop ((i 0))
  (if (< i NPTS)
      (begin (view-set! xs i (+ -2.0 (* (/ 4.0 NPTS) i)))
             (loop (+ i 1)))))

;; ONE scratch buffer, rewritten in place for every particle. `batch` reads
;; it and sums immediately, so nothing outlives the call.
(define scratch (bytes-view (make-bytes (* NPTS 8)) :f64))

(define (curve-at a b c)
  (let loop ((i 0))
    (if (= i NPTS)
        scratch
        (let ((x (view-ref xs i)))
          (view-set! scratch i (+ (* a x x) (* b x) c))
          (loop (+ i 1))))))

(define ys (bytes-view (make-bytes (* NPTS 8)) :f64))
(rng-fill-normal! (rng-make 0 9 0) ys 0 NPTS (curve-at A0 B0 C0) NOISE)

;;--- the model ----------------------------------------------------------
;; The 1.5 is the prior's width on each coefficient — how vague the model
;; is before it sees data. It is written here rather than hoisted into a
;; constant because this is where you want to argue with it.

(define-gen (curve)
  (let* ((a (at :a (normal 0 1.5)))
         (b (at :b (normal 0 1.5)))
         (c (at :c (normal 0 1.5))))
    (at :ys (batch (normal (curve-at a b c) SIGMA) NPTS))))

;;--- the fit ------------------------------------------------------------
;; The browser stops a top-level evaluation after a fixed slice so that a
;; runaway program cannot freeze the tab, which is right for a typo and
;; wrong for a computation that is expensive on purpose. Only the program
;; knows which it is, so it says so. eval-budget-ms! returns the previous
;; value, and unwind-protect puts it back even if the fit raises.
;;
;; Restoring sets a FRESH slice rather than the old instant: by now the
;; original deadline has passed, and handing it back would time the
;; program out during its own plotting.

;; Timed around the fit alone, not the whole script: the plotting is a
;; couple of milliseconds and constant, so folding it in would blur the one
;; number that actually tracks K.
(define fit
  (time (lambda ()
          (let ((was (eval-budget-ms! 8000)))
            (unwind-protect
              (importance (curve) {:ys ys} K SEED)
              (eval-budget-ms! was))))))

(define soa (cdr fit))
(define ms  (* 1000.0 (car fit)))

(define ws (:weights soa))
(normalize-weights! ws K)              ; log-weights -> probabilities

(define ess
  (let loop ((i 0) (s 0.0))
    (if (= i K)
        (/ 1.0 s)
        (loop (+ i 1) (+ s (* (view-ref ws i) (view-ref ws i)))))))

;; NDRAW particles chosen with probability proportional to weight. A
;; duplicate is information, not waste: it means that particle carried
;; several curves' worth of belief, and drawing duplicates on top of each
;; other is what makes a concentrated posterior LOOK concentrated. Taking
;; the best NDRAW distinct particles instead would show the same spread
;; whether the weights were even or degenerate.
(define picks (bytes-view (make-bytes (* NDRAW 4)) :i32))
(rng-fill-categorical! (rng-make 0 SEED 0) picks 0 NDRAW ws)

;;--- the picture --------------------------------------------------------

(load "lib/fitplot.scm")
(plot-fit! xs ys NPTS soa picks (list A0 B0 C0) ess)

(define (1dp x) (/ (round (* 10.0 x)) 10.0))

(display (format "ESS ~a of ~a particles in ~a ms\n"
                 (inexact->exact (round ess)) K (1dp ms)))

;; us-per-particle is the number to plan with: multiply by the K you are
;; considering and compare against the budget above before you get cut off.
(list (cons 'ess ess)
      (cons 'K K)
      (cons 'ms (1dp ms))
      (cons 'us-per-particle (1dp (/ (* 1000.0 ms) K))))
