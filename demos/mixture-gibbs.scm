;;; ==========================================================
;;; A Gibbs sweep over a mixture of Gaussians
;;; ==========================================================
;;; What this is for, beyond showing means converge: it is the CALLER the
;;; kernel does not have yet. Building it first says what a device half
;;; would be asked for, rather than guessing at a signature and finding
;;; out after a second dialect is already built on it.
;;;
;;; THE MODEL IS STAGEABLE, which it was not until a computed index was
;;; allowed. The assignments arrive as DATA -- a sweep hands in the current
;;; z each iteration -- so the likelihood reads mu[z[i]], a gather, and the
;;; means are the batched choice being sampled.
;;;
;;; THE MEAN STEP IS HAND WRITTEN. lib/gibbs.scm refuses a batched choice
;;; ("a batched choice has no scalar conjugate update"), which is the other
;;; half of MANUAL.md's sentence about conjugacies that need an assignment.
;;; Extending it is a real feature and not on the path to a device.
;;;
;;; WHAT THE SWEEP ASKS FOR, which is the point:
;;;   per iteration, changing   mu (K floats), z (N floats)
;;;   per iteration, fixed      ys (N floats), sigma, w (K floats)
;;;   the hot quantity          N*K log-weights -- NOT the log-joint
;;;   the cheap quantity        one scalar log-joint, for monitoring
;;; The second is what the staged IR describes. The first is not, and that
;;; is the finding: a Gibbs assignment step wants the likelihood's SUMMAND
;;; for every (i, k), unreduced, where the IR says "sum over i".

(load "lib/stage.scm")

;;--- the problem --------------------------------------------------------

(define K 3)
(define N 300)
(define SIGMA 0.7)
(define PRIOR-SD 10.0)
(define SWEEPS 24)
(define TRUE-MU '(-3.0 0.0 4.0))

(define r (rng-make 0 20261008 0))

(define ones (bytes-view (make-bytes (* K 8)) :f64))
(let loop ((c 0)) (if (< c K) (begin (view-set! ones c 1.0) (loop (+ c 1)))))

(define ys (bytes-view (make-bytes (* N 8)) :f64))
(let loop ((i 0))
  (if (< i N)
      (let ((c (inexact->exact (random-categorical r ones))))
        (view-set! ys i (random-normal r (list-ref TRUE-MU c) SIGMA))
        (loop (+ i 1)))))

;;--- the model, as garyo will read it -----------------------------------

(define-gen (mixture zs sigma n k)
  (let ((m (at :mu (batch-i k (c) (normal 0.0 10.0)))))
    (at :ys (batch-i n (i) (normal (view-ref m (view-ref zs i)) sigma)))))

;;--- state --------------------------------------------------------------

(define zs (bytes-view (make-bytes (* N 8)) :f64))
(define mu (bytes-view (make-bytes (* K 8)) :f64))
(define lw (bytes-view (make-bytes (* K 8)) :f64))

;; Deliberately a bad start: every point in component 0 and every mean at
;; zero, so the sweep has somewhere to travel from.
(let loop ((i 0)) (if (< i N) (begin (view-set! zs i 0.0) (loop (+ i 1)))))
(let loop ((c 0)) (if (< c K) (begin (view-set! mu c 0.0) (loop (+ c 1)))))

;;--- the assignment step: O(N*K), and the kernel's job ------------------
;; Weights are exponentiated relative to the largest, which keeps them in
;; range; random-categorical does not need them normalised.

(define (sample-assignments!)
  (let per-point ((i 0))
    (if (< i N)
        (let ((y (view-ref ys i)))
          (let weigh ((c 0) (best -inf))
            (if (< c K)
                (let ((l (logpdf-normal y (view-ref mu c) SIGMA)))
                  (view-set! lw c l)
                  (weigh (+ c 1) (if (> l best) l best)))
                (begin
                  (let rescale ((c 0))
                    (if (< c K)
                        (begin (view-set! lw c (exp (- (view-ref lw c) best)))
                               (rescale (+ c 1)))))
                  (view-set! zs i (random-categorical r lw)))))
          (per-point (+ i 1))))))

;;--- the mean step: conjugate normal-normal, per component --------------
;; Prior N(0, PRIOR-SD), likelihood N(mu_c, SIGMA) over the points assigned
;; to c. Precisions add; the posterior mean is the precision-weighted sum.

(define (sample-means!)
  (let per-component ((c 0))
    (if (< c K)
        (let tally ((i 0) (n 0) (s 0.0))
          (if (< i N)
              (if (= (view-ref zs i) c)
                  (tally (+ i 1) (+ n 1) (+ s (view-ref ys i)))
                  (tally (+ i 1) n s))
              (let* ((prec (+ (/ 1.0 (* PRIOR-SD PRIOR-SD))
                              (/ n (* SIGMA SIGMA))))
                     (m    (/ (/ s (* SIGMA SIGMA)) prec)))
                (view-set! mu c (random-normal r m (/ 1.0 (sqrt prec))))
                (per-component (+ c 1))))))))

;;--- run ----------------------------------------------------------------

(define st (stage (mixture zs SIGMA N K)))
(define (log-joint) (staged-logpdf st {:mu mu :ys ys}))

(define (r3 x) (/ (round (* 1000.0 x)) 1000.0))

(display "a mixture of ") (display K) (display " gaussians, ")
(display N) (display " points, sigma ") (display SIGMA) (newline)
(display "true means ") (write TRUE-MU) (newline) (newline)
(display "sweep      mu_0     mu_1     mu_2    log-joint") (newline)

(let sweep ((t 0))
  (if (<= t SWEEPS)
      (begin
        (if (> t 0) (begin (sample-assignments!) (sample-means!)))
        (display "  ") (display t)
        (display (if (< t 10) "     " "    "))
        (let show ((c 0))
          (if (< c K)
              (begin (display (r3 (view-ref mu c))) (display "   ") (show (+ c 1)))))
        (display "  ") (display (r3 (log-joint))) (newline)
        (sweep (+ t 1)))))

(newline)
(display "recovered, sorted: ")
(write (let sort3 ((xs (list (view-ref mu 0) (view-ref mu 1) (view-ref mu 2))))
         (map r3 (list (apply min xs)
                       (- (apply + xs) (apply min xs) (apply max xs))
                       (apply max xs)))))
(newline)
(display "true,      sorted: ") (write TRUE-MU) (newline)

;;--- hand the final state to garyo --------------------------------------
;; The whole point of the boundary being text: the model and the state it
;; reached go out as one string each, and a reader outside this VM scores
;; them. If the two numbers differ, one of us read the IR wrong.

(define (view->list v n)
  (let loop ((j (- n 1)) (acc '()))
    (if (< j 0) acc (loop (- j 1) (cons (view-ref v j) acc)))))

(call-with-output-file "/tmp/sweep.ir"
  (lambda (p) (write (staged-export st) p) (newline p)))

(call-with-output-file "/tmp/sweep.fixture"
  (lambda (p)
    (write {:scalars '()
            :batched (list (list :mu (view->list mu K))
                           (list :ys (view->list ys N)))
            :buffers (list (list 'zs (view->list zs N)))
            :logpdf-f64 (log-joint)
            :logpdf-f32 (with-staged-f32 (log-joint))}
           p)
    (newline p)))

(newline)
(display "wrote /tmp/sweep.ir and /tmp/sweep.fixture") (newline)
(display "vxs log-joint at the final state: ") (write (log-joint)) (newline)
