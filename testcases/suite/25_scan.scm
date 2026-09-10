;;----------------------------------------------------------------------
;; Layer 25: scan-with-state — a trajectory staged from its source
;;
;; batch-i maps: its j-th choice is computable from j alone. A trajectory
;; cannot be, because x_j depends on x_{j-1}, so it scans instead — the
;; fold carries the state and the running score together.
;;
;; The vehicle is the NON-LINEARISED simple pendulum, and it is a test
;; vehicle rather than a demonstration. It is here because it comes with
;; an ORACLE IN CLOSED FORM: the period of a pendulum of amplitude θ₀ is
;; exactly
;;
;;     T = 4 √(L/g) · K(sin(θ₀/2))
;;
;; with K the complete elliptic integral of the first kind, which the
;; arithmetic-geometric mean computes to full precision in four or five
;; iterations. So the integrator is checked against TRUTH rather than
;; against a finer approximation of itself — the same standing as gradient
;; noise being exactly zero at lattice points.
;;
;; The linearised pendulum has an amplitude-independent period. The real
;; one does not: +1.7% at 30°, +18% at 90°, +37% at 120°. That deviation
;; is what makes this a real test of the integrator rather than a check
;; that a cosine is a cosine.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")
(load "lib/stage.scm")

(test-suite "25_scan: a trajectory staged from its source")

;;--- the oracle ---------------------------------------------------------
;; K(k) = π / (2·AGM(1, √(1-k²))). The AGM converges quadratically, so the
;; loop below is doing four or five iterations, not an approximation whose
;; error needs arguing about.

(define (agm a b)
  (let loop ((a a) (b b) (i 0))
    (if (or (= i 12) (< (abs (- a b)) 1e-15))
        a
        (loop (* 0.5 (+ a b)) (sqrt (* a b)) (+ i 1)))))

(define (elliptic-K k) (/ dist-pi (* 2.0 (agm 1.0 (sqrt (- 1.0 (* k k)))))))

;; The exact period, in units where the linearised period is 2π/w.
(define (pendulum-period w theta0)
  (/ (* 4.0 (elliptic-K (sin (* 0.5 theta0)))) w))

;; K(0) is π/2 exactly — the linear limit, where the period stops
;; depending on amplitude.
(assert-true "the AGM reproduces K(0) = pi/2"
             (< (abs (- (elliptic-K 0.0) (/ dist-pi 2.0))) 1e-14))
;; Published: K(m=1/2) = 1.8540746773013719...
(assert-true "and K at k = sin(45 degrees), against the published value"
             (< (abs (- (elliptic-K (sin (* 0.25 dist-pi))) 1.8540746773013719)) 1e-13))
;; The headline nonlinearity: an amplitude of 90 degrees stretches the
;; period by 18%, which no linearised model can produce at any frequency.
(assert-true "a 90-degree swing has a period 18% longer than the linear one"
             (< (abs (- (/ (pendulum-period 1.0 (* 0.5 dist-pi)) (* 2.0 dist-pi))
                        1.1803405990)) 1e-8))

;;--- the integrator, as a pair of kernel citizens ------------------------
;; One classical RK4 step of  θ' = ω,  ω' = -w² sin θ.
;;
;; The stages are computed twice, once per component, because a kernel
;; function returns one value and the scan form takes one expression per
;; component. That is a real cost of the shape — about a factor of two in
;; the integrator — and it is written down rather than worked around,
;; since the alternative is a state bundle the type system does not have.

(define-dual (rk4-theta (th :f32) (om :f32) (w :f32) (h :f32))
  (let* ((k1t om)                          (k1o (* (- 0.0 (* w w)) (sin th)))
         (k2t (+ om (* 0.5 h k1o)))        (k2o (* (- 0.0 (* w w)) (sin (+ th (* 0.5 h k1t)))))
         (k3t (+ om (* 0.5 h k2o)))        (k3o (* (- 0.0 (* w w)) (sin (+ th (* 0.5 h k2t)))))
         (k4t (+ om (* h k3o))))
    (+ th (* (/ h 6.0) (+ k1t (* 2.0 k2t) (* 2.0 k3t) k4t)))))

(define-dual (rk4-omega (th :f32) (om :f32) (w :f32) (h :f32))
  (let* ((k1t om)                          (k1o (* (- 0.0 (* w w)) (sin th)))
         (k2t (+ om (* 0.5 h k1o)))        (k2o (* (- 0.0 (* w w)) (sin (+ th (* 0.5 h k1t)))))
         (k3t (+ om (* 0.5 h k2o)))        (k3o (* (- 0.0 (* w w)) (sin (+ th (* 0.5 h k2t)))))
         (k4o (* (- 0.0 (* w w)) (sin (+ th (* h k3t))))))
    (+ om (* (/ h 6.0) (+ k1o (* 2.0 k2o) (* 2.0 k3o) k4o)))))

;; Integrate until θ changes sign, and interpolate the crossing. A quarter
;; period elapses between the peak and the first zero, so four crossings
;; from rest is one full period.
(define (quarter-period w theta0 h)
  (let loop ((th theta0) (om 0.0) (t 0.0) (i 0))
    (if (= i 2000000)
        (error 'pendulum "no crossing")
        (let ((th2 (rk4-theta th om w h))
              (om2 (rk4-omega th om w h)))
          (if (< (* th th2) 0.0)
              (+ t (* h (/ th (- th th2))))     ; linear interpolation
              (loop th2 om2 (+ t h) (+ i 1)))))))

;; The whole point of the layer's oracle. A LINEARISED integrator would
;; pass this at small amplitude and fail badly at large, so it tests the
;; nonlinearity and not merely the arithmetic.
(define (period-error theta0)
  (let* ((w 1.0)
         (h 0.0005)
         (measured (* 4.0 (quarter-period w theta0 h)))
         (exact    (pendulum-period w theta0)))
    (abs (/ (- measured exact) exact))))

(assert-true "the integrator's period matches the closed form at 30 degrees"
             (< (period-error (* dist-pi (/ 30.0 180.0))) 1e-6))
(assert-true "at 90 degrees, where the linear model is already 18% wrong"
             (< (period-error (* 0.5 dist-pi)) 1e-6))
(assert-true "and at 150 degrees, near the slow approach to the top"
             (< (period-error (* dist-pi (/ 150.0 180.0))) 1e-6))

;;--- the model ----------------------------------------------------------
;; The state is (θ, ω); the observation is a noisy angle. Written with
;; scan-i, which is what a compiler can read.

(define NOBS 12)
(define ys (bytes-view (make-bytes (* NOBS 8)) :f64))
(rng-fill-normal! (rng-make 0 5 0) ys 0 NOBS 0.0 0.4)

(define-gen (pendulum sigma nobs th0 h)
  (let ((w (at :w (uniform 0.5 4.0))))
    (at :ys (scan-i nobs (j)
              ((th th0 (rk4-theta th om w h))
               (om 0.0 (rk4-omega th om w h)))
              (normal th sigma)))))

(define st (stage (pendulum 0.4 NOBS 1.0 0.05)))

(assert-equal "the scan's address is a batched choice like any other"
              '((:w . scalar) (:ys batched 12)) (:choices st))
(assert-equal "and it lowered to a scan carrying two state components"
              '(th om)
              (map car (cadddr (cadr (:terms st)))))

;;--- backend one: against assess ----------------------------------------

(define (agree? w)
  (let ((ch {:w w :ys ys}))
    (abs (- (car (assess (pendulum 0.4 NOBS 1.0 0.05) ch))
            (staged-logpdf st ch)))))

(assert-true "the staged log-joint agrees with assess"  (< (agree? 1.0) 1e-12))
(assert-true "at a faster pendulum"                     (< (agree? 3.7) 1e-12))
(assert-true "and a slower one"                         (< (agree? 0.6) 1e-12))
(assert-equal "bit-for-bit, as with the mapped case"    0.0 (agree? 2.2))

;; The reason the two agree is not luck: the scan is genuinely carrying
;; state, so a model whose state never advanced would give a different
;; answer. This pins that down — an amplitude the pendulum only reaches by
;; swinging there.
(assert-true "the trajectory really moves: a still pendulum scores differently"
             (> (abs (- (staged-logpdf st {:w 1.0 :ys ys})
                        (staged-logpdf st {:w 0.0001 :ys ys})))
                1.0))

;;--- backend two: the device --------------------------------------------

(wgsl-declare! 'logpdf-normal "logpdf_normal" '(:f32 :f32 :f32) :f32)
(wgsl-declare! 'logpdf-uniform "logpdf_uniform" '(:f32 :f32 :f32) :f32)
(wgsl-declare! 'ys "ys_at" '(:u32) :f32)

(assert-equal "the emitted kernel expression type-checks to a scalar"
              :f32 (wgsl-type (staged-kernel st) '((w . :f32))))

(set! wgsl-counter 0)
(define ksrc (wgsl-body (staged-kernel st) '((w . :f32)) ""))

;; Two state components plus the score is three lanes, so the accumulator
;; is a vec3 — the packing the ceiling is about.
(assert-true "the accumulator packs state and score into one vector"
             (string-contains? ksrc "vec3<f32>"))
(assert-true "which is seeded from the initial state, with the score at zero"
             (string-contains? ksrc "vec3<f32>(1.0, 0.0, 0.0)"))
(assert-true "the state is unpacked at the top of each pass"
             (string-contains? ksrc ".x;"))
(assert-true "and the integrator is called by its underscored name"
             (string-contains? ksrc "rk4_theta("))

;;--- the ceiling, and the other refusals --------------------------------

(define-gen (four-state h)
  (at :v (scan-i 3 (j)
           ((a 0.0 (+ a h)) (b 0.0 (+ b h)) (c 0.0 (+ c h)) (d 0.0 (+ d h)))
           (normal a 1.0))))
(assert-true "four state components are refused: the fold's vector holds the score too"
             (guard (e (#t #t)) (stage (four-state 0.1)) #f))

(wgsl-declare! 'v "v_at" '(:u32) :f32)
(define-gen (three-state h)
  (at :v (scan-i 3 (j)
           ((a 0.0 (+ a h)) (b 0.0 (+ b h)) (c 0.0 (+ c h)))
           (normal a 1.0))))
(assert-true "three are accepted, packing into a vec4"
             (string-contains? (wgsl-body (staged-kernel (stage (three-state 0.1)))
                                          '() "")
                               "vec4<f32>"))
;; The score rides in the last lane, so a three-state scan reads it out of
;; w where the pendulum's two-state scan reads it out of z.
(assert-equal "and the score still comes back out as a scalar"
              :f32 (wgsl-type (staged-kernel (stage (three-state 0.1))) '()))

(define-gen (indexes-by-state xs)
  (at :v (scan-i 3 (j) ((a 0.0 (+ a 1.0))) (normal (view-ref xs a) 1.0))))
(assert-true "a buffer indexed by a state value is refused — that is a gather"
             (guard (e (#t #t)) (stage (indexes-by-state ys)) #f))

(define-gen (empty-state)
  (at :v (scan-i 3 (j) () (normal 0.0 1.0))))
(assert-true "a scan with no state is refused: that is a batch-i"
             (guard (e (#t #t)) (stage (empty-state)) #f))

;;--- the steps see the OLD state ----------------------------------------
;; The semantics that would be silently wrong if it were let* instead of
;; let: threading the components would turn an explicit integrator into a
;; semi-implicit one, which still converges and still looks plausible.
;;
;; With a := a + 1 and b := a (both from the old state), after three passes
;; b lags a by exactly one. Sequential update would make them equal.

(define-gen (order-probe)
  (at :v (scan-i 3 (j) ((a 0.0 (+ a 1.0)) (b 0.0 a)) (normal b 1.0))))

(define probe (stage (order-probe)))
;; b takes the values 0, 0, 1 across the three passes.
(assert-true "every step reads the state as it stood, not as another step left it"
             (< (abs (- (staged-logpdf probe {:v (let ((v (bytes-view (make-bytes 24) :f64)))
                                                   (view-set! v 0 0.0)
                                                   (view-set! v 1 0.0)
                                                   (view-set! v 2 1.0)
                                                   v)})
                        (* 3.0 (logpdf-normal 0.0 0.0 1.0))))
                1e-12))

(suite-summary)
