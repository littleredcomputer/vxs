;;----------------------------------------------------------------------
;; Layer 23: generative functions over fibers (lib/gen.scm)
;;
;; A model is an ordinary procedure that calls `at` where it makes a
;; random choice. Nothing about it is transformed or declared — `at`
;; yields, and a driver on the other side decides what the choice is
;; worth. Everything asserted here defends that arrangement.
;;
;; These began as a scratch file's display statements. Several of the bugs
;; they now pin were found by reading that output and noticing a number
;; was plausible rather than right, which is why the assertions below
;; check ARITHMETIC against closed forms wherever one exists: a batched
;; score that silently reports log(1-p) for four flips looks entirely
;; reasonable until you compute what it should have been.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")
(load "lib/gen.scm")

(test-suite "23_gen: generative functions over fibers")

(define k0 (rng-make 0 0 0))

;;--- distributions as records -------------------------------------------

(define n0 (normal 10.0 1.0))
(assert-equal "a distribution knows its form" '(normal 10.0 1.0) (dist-form n0))
(assert-equal "and is recognised" '(#t #f) (list (distribution? n0) (distribution? 5)))

;; The record is the point: a mistyped ACCESSOR is an unbound variable at
;; compile time, where a mistyped MESSAGE would fall to an else clause at
;; run time. Nothing to assert about a compile error, but the positive
;; case shows the shape.
(assert-equal "scoring uses the closed-over parameters"
              #t (< (abs (- (d:score n0 10.0) (logpdf-normal 10.0 10.0 1.0))) 1e-12))

;;--- batch --------------------------------------------------------------
;; Converts (fill, sum) into (sample, score) at a larger shape, and is
;; itself a distribution — which is what lets it sit at an address.

(define b7 (batch (flip 0.5) 7))
(assert-equal "a batched distribution reports what it wraps"
              '(batch (flip 0.5) 7) (dist-form b7))
(assert-equal "and is still a distribution" #t (distribution? b7))

(define bv (d:sample (batch (normal 0.0 1.0) 16) (rng-make 0 5 0)))
(assert-equal "sampling one gives a view of that many" 16 (view-length bv))

;; The score of a batch is the SUM over its elements — one scalar, not a
;; vector. This is the bug that hid longest: omitting the shape made the
;; scalar scorer run on a view, and arithmetic leniency turned that into
;; log(1-p), a number indistinguishable from a plausible answer.
(define fb (d:sample (batch (flip 0.25) 200) (rng-make 0 9 0)))
(assert-equal "a batch scores as the sum over its elements"
              #t (< (abs (- (d:score (batch (flip 0.25) 200) fb)
                            (logpdf-sum-flip fb 0 200 0.25)))
                    1e-9))

;; What it does NOT have, stated in the constructor rather than
;; discovered by falling through: a batched distribution has no fill or
;; sum, so batch is not closed under itself.
(assert-equal "a batched distribution refuses fill"
              'raised (guard (e (#t 'raised)) (d:fill! b7 k0 bv 0 7)))
(assert-equal "so batch cannot be nested"
              'raised (guard (e (#t 'raised))
                        (d:sample (batch (batch (flip 0.5) 3) 2) k0)))

;;--- at, and calling a model directly ------------------------------------
;; (yield) outside a generator is LEGAL — the scheduler resumes it with
;; unspecified — so `at` has to refuse on its own behalf. Without this a
;; model called directly runs, receives unspecified for every choice, and
;; returns a plausible number, because arithmetic does not type-check.

(define (bare-model) (at :x (normal 0 1)))
(assert-equal "at refuses to run outside a generative function"
              'raised (guard (e (#t 'raised)) (bare-model)))

;;--- simulating ----------------------------------------------------------

(define-gen (two-choice v)
  (let* ((x (at :x (normal 0 1)))
         (z (at :z (uniform v (+ 10 v)))))
    (+ x z)))

(define tr (sample (two-choice 20) 1))

(assert-equal "a trace has an entry per address"
              '(#t #t) (list (map-has? tr :x) (map-has? tr :z)))
(assert-equal "each carrying a value and its score"
              '(#t #t) (list (map-has? (:x tr) :retval) (map-has? (:x tr) :score)))
(assert-equal "the return value is the model's"
              #t (< (abs (- (:retval tr) (+ (:retval (:x tr)) (:retval (:z tr))))) 1e-12))
(assert-equal "and the score is the sum of the choices'"
              #t (< (abs (- (:score tr) (+ (:score (:x tr)) (:score (:z tr))))) 1e-12))
(assert-equal "each score is its own logpdf"
              #t (< (abs (- (:score (:x tr)) (logpdf-normal (:retval (:x tr)) 0 1))) 1e-12))

;; Reproducible from the seed, and only from the seed.
(assert-equal "the same seed gives the same trace"
              (:retval (sample (two-choice 20) 1)) (:retval (sample (two-choice 20) 1)))
(assert-equal "a different seed does not"
              #f (= (:retval (sample (two-choice 20) 1))
                    (:retval (sample (two-choice 20) 2))))

;;--- assessing -----------------------------------------------------------
;; Draws nothing; every address must be constrained.

(define aw (assess (two-choice 20) {:x 1.1 :z 21.0}))
(assert-equal "assess returns (weight . retval)" 22.1 (cdr aw))
(assert-equal "and the weight is the sum of the constrained logpdfs"
              #t (< (abs (- (car aw)
                            (+ (logpdf-normal 1.1 0 1) (logpdf-uniform 21.0 20 30))))
                    1e-12))
(assert-equal "a missing choice is a caller error, not a zero probability"
              'raised (guard (e (#t 'raised)) (assess (two-choice 20) {:x 1.1})))

;;--- a batched choice inside a model --------------------------------------

(define-gen (coin n)
  (let ((p (at :p (uniform 0 1))))
    (at :qs (batch (flip p) n))))

(define ct (sample (coin 4) 3))
(assert-equal "a batched address holds a view" 4 (view-length (:retval (:qs ct))))
;; The score of the batch must be the SUM over its four flips, computed
;; against the p that was actually drawn. Checked against the closed form
;; because the failure mode was a plausible number, not an error.
(assert-equal "scored against the p that was drawn"
              #t (< (abs (- (:score (:qs ct))
                            (logpdf-sum-flip (:retval (:qs ct)) 0 4 (:retval (:p ct)))))
                    1e-9))

;;--- nesting --------------------------------------------------------------
;; A generative function at an address. The sub-TRACE goes at the address;
;; the parent is resumed with the sub-model's RETURN VALUE. Those are
;; different things, and returning the trace where the value belongs makes
;; the parent multiply a number by a map — silently, again.

(define-gen (outer)
  (let* ((v (at :v (uniform -4 4)))
         (w (at :w (two-choice v))))
    (* v w)))

(define ot (sample (outer) 42))

(assert-equal "a nested address holds a whole sub-trace"
              '(#t #t) (list (map-has? (:w ot) :x) (map-has? (:w ot) :score)))
(assert-equal "the parent is resumed with the sub-model's return value"
              #t (< (abs (- (:retval ot) (* (:retval (:v ot)) (:retval (:w ot))))) 1e-12))
(assert-equal "and the sub-trace's score is added to the parent's"
              #t (< (abs (- (:score ot) (+ (:score (:v ot)) (:score (:w ot))))) 1e-12))

;; The sub-model draws from a SPLIT generator, so it is insulated from its
;; siblings' draw counts — the property rng-split! exists for.
(assert-equal "a nested trace is reproducible from the outer seed"
              (:retval (sample (outer) 42)) (:retval (sample (outer) 42)))

;; Assessing a nested model needs hierarchical constraints, and a missing
;; SUB-MAP must be reported at its own level. Without the check it reports
;; the first leaf inside, naming a grandchild when the child is missing.
(define ao (assess (outer) {:v 2 :w {:x 1.2 :z 7}}))
(assert-equal "nested assess returns the product"
              #t (< (abs (- (cdr ao) (* 2 (+ 1.2 7)))) 1e-12))
(assert-equal "and sums the inner weights into the outer"
              #t (< (abs (- (car ao)
                            (+ (logpdf-uniform 2 -4 4)
                               (logpdf-normal 1.2 0 1)
                               (logpdf-uniform 7 2 12))))
                    1e-12))
(assert-equal "a missing sub-map is reported at its own address"
              '(missing-choice :w)
              (guard (e (#t e)) (assess (outer) {:v 2})))


;;--- importance ---------------------------------------------------------
;; K samples, transposed: columns rather than K traces. The arithmetic is
;; checked against a closed form because that is the only way to catch the
;; failure this code is prone to — a weight that is plausible and wrong.

(define obs (bytes-view (make-bytes (* 10 4)) :f32))
(let loop ((i 0))
  (if (< i 10) (begin (view-set! obs i (if (< i 3) 1.0 0.0)) (loop (+ i 1)))))

(define KK 8000)
(define soa (importance (coin 10) {:qs obs} KK 1))

(assert-equal "the result is columns, not traces"
              '(#t #t) (list (map-has? soa :p) (map-has? soa :weights)))
(assert-equal "one column entry per sample" KK (view-length (:p soa)))
(assert-equal "and one weight per sample"   KK (view-length (:weights soa)))
;; The CONSTRAINED address is not a column — it is the same for every
;; sample, so a column would store K copies of a fact already in the
;; constraints. That is the whole argument for transposing.
(assert-equal "a constrained address gets no column" #f (map-has? soa :qs))

;; THE assertion. Three heads in ten under a uniform prior is Beta(4,8),
;; whose mean is exactly 1/3. Anything that put the unconstrained draw's
;; density into the weight would still concentrate somewhere — just not
;; here.
(assert-equal "the posterior mean of p is Beta(4,8)'s"
              #t (< (abs (- (weighted-mean (:p soa) (:weights soa) KK) (/ 1.0 3.0)))
                    0.02))

;; The weight is the log density of the CONSTRAINED part only, given the
;; drawn latent — not the trace's total score.
(assert-equal "each weight is the constrained logpdf alone"
              #t (< (abs (- (view-ref (:weights soa) 0)
                            (logpdf-sum-flip obs 0 10 (view-ref (:p soa) 0))))
                    1e-9))

;; Sample i draws from (rng-make i seed 0), so a sample does not depend on
;; how many were taken or in what order.
(assert-equal "sample i is the same at any K"
              (view-ref (:p (importance (coin 10) {:qs obs} 50 1)) 37)
              (view-ref (:p (importance (coin 10) {:qs obs} 900 1)) 37))

(assert-equal "normalised weights sum to one"
              #t (let ((ws (:weights (importance (coin 10) {:qs obs} 500 2))))
                   (normalize-weights! ws 500)
                   (let loop ((i 0) (s 0.0))
                     (if (= i 500) (< (abs (- s 1.0)) 1e-9)
                         (loop (+ i 1) (+ s (view-ref ws i)))))))

;; Two shapes refused rather than fudged, each naming its own reason.
(assert-equal "a nested generative function is refused for now"
              'raised (guard (e (#t 'raised)) (importance (outer) {:v 1} 4 1)))
(assert-equal "and an unconstrained batched choice"
              'raised (guard (e (#t 'raised)) (importance (coin 10) {} 4 1)))

(suite-summary)
