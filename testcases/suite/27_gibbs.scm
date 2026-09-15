;;----------------------------------------------------------------------
;; Layer 27: conjugate structure, read from the staged IR
;;
;; lib/gibbs.scm asks whether a model HAS a closed-form conditional at an
;; address, and recovers its coefficients if it does. The claim under test
;; is that the answer is a PROOF FROM THE TEXT rather than an observation
;; of a run.
;;
;; WHY THAT MATTERS MORE HERE THAN ANYWHERE ELSE IN THE SYSTEM. Everything
;; else staged can be checked by comparing two backends: if the reading was
;; wrong, the numbers disagree and the oracle says so. A Gibbs update
;; cannot be checked that way. It is derived from a structural CLAIM — this
;; prior has these children, entering their means this way — and a second
;; derivation from the same claim repeats the same mistake, so both sides
;; agree with each other while the chain converges to the wrong stationary
;; distribution. Two-path detection assumes the paths fail independently,
;; and here they do not.
;;
;; WHICH IS NOT THE SAME AS UNCHECKABLE, and it took a wrong turn to see
;; it. The claim divides in two, and each half has a tool that can see it.
;; THAT the conditional is Gaussian is a property of the text, proved by
;; the refusals below. WHICH Gaussian it is, is then a property of the
;; model and can be measured: a Gaussian conditional makes the log-joint
;; exactly quadratic in the address, so three evaluations of staged-logpdf
;; recover the mean and scale as an IDENTITY — no fit, no residual, no
;; step-size to tune — by a route that touches no coefficient the
;; derivation produced. That is a genuine oracle, and the update is
;; checked against it below to 1e-11.
;;
;; So the negatives here are refusals rather than tolerances because the
;; first half admits no measurement, not because the whole thing doesn't.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")
(load "lib/gibbs.scm")

(test-suite "27_gibbs: conjugate structure from the IR")

(define (refused? thunk) (guard (e (#t #t)) (thunk) #f))

;;--- the model ----------------------------------------------------------

(define-dual (curve-elem (x :f32) (a :f32) (b :f32) (c :f32))
  (+ (* a x x) (* b x) c))

(define NPTS 4)

(define xs (bytes-view (make-bytes (* NPTS 8)) :f64))
(define ys (bytes-view (make-bytes (* NPTS 8)) :f64))
(let loop ((j 0))
  (if (< j NPTS)
      (begin (view-set! xs j (* 0.5 j))
             (view-set! ys j (* 0.1 j))
             (loop (+ j 1)))))

(define-gen (curve xs sigma npts)
  (let* ((a (at :a (normal 0 1.5)))
         (b (at :b (normal 0 1.5)))
         (c (at :c (normal 0 1.5))))
    (at :ys (batch-i npts (j)
              (normal (curve-elem (view-ref xs j) a b c) sigma)))))

(define st (stage (curve xs 1.0 NPTS)))

;;--- what was read ------------------------------------------------------
;; The mean is (call curve-elem ...) in the IR, so every coefficient below
;; came out of the dual's BODY. Nothing about the affine structure is
;; visible from the call site.

(define ga (gibbs-structure st :a))
(define gb (gibbs-structure st :b))
(define gc (gibbs-structure st :c))

(assert-equal "the pair is recognised"      'normal-normal (:kind ga))
(assert-equal "the prior is read as written" '(0 1.5)      (:prior ga))
(assert-equal "with one child term"          1 (length (:children ga)))

;; mean = a*x*x + b*x + c, so the coefficient of a is x*x and everything
;; else is the offset.
(assert-equal "the coefficient of :a is read out of the helper's body"
              '(* (data xs j) (data xs j))
              (caddr (car (:children ga))))
(assert-equal "and the rest of the mean becomes the offset"
              '(+ (* (choice :b) (data xs j)) (choice :c))
              (cadddr (car (:children ga))))

(assert-equal "the coefficient of :b is the design, not its square"
              '(data xs j)
              (caddr (car (:children gb))))

;; The identities are folded, so the intercept is exactly 1 rather than
;; (* 1.0 something). A caller can therefore recognise the simple case.
(assert-equal "an intercept has coefficient exactly one"
              1 (caddr (car (:children gc))))

(assert-equal "the child keeps the bound and index of its reduction"
              (list 4 'j)
              (list (car (car (:children ga))) (cadr (car (:children ga)))))
(assert-equal "and what it scores"
              '(choice-i :ys j)
              (list-ref (car (:children ga)) 5))

;; Another address's value in the offset is CORRECT rather than leakage: a
;; Gibbs conditional holds every other address fixed, so (choice :b)
;; appearing in :a's offset is a conditioned constant.
(assert-true "another choice may appear in the offset"
             (ir-mentions? (cadddr (car (:children ga))) :b))

;;--- the affine reader on its own ---------------------------------------

(assert-equal "an address alone is coefficient one, offset zero"
              '(1 . 0) (ir-affine '(choice :a) :a))
(assert-equal "an expression without it is all offset"
              '(0 . (data xs j)) (ir-affine '(data xs j) :a))
(assert-equal "a scale factor becomes the coefficient"
              '((data xs j) . 0) (ir-affine '(* (choice :a) (data xs j)) :a))
(assert-equal "a negation carries through"
              '(-1 . 0) (ir-affine '(- (choice :a)) :a))
(assert-equal "and a division by a constant divides both parts"
              '((/ 1 2.0) . (/ (choice :b) 2.0))
              (ir-affine '(/ (+ (choice :a) (choice :b)) 2.0) :a))

;;--- everything else is refused by name ---------------------------------

(define-gen (quad npts)
  (let ((a (at :a (normal 0 1.0))))
    (at :ys (batch-i npts (j) (normal (* a a) 1.0)))))
(assert-true "a mean quadratic in the address is refused, not linearised"
             (refused? (lambda () (gibbs-structure (stage (quad NPTS)) :a))))

(define-gen (inscale npts)
  (let ((a (at :a (normal 0 1.0))))
    (at :ys (batch-i npts (j) (normal 0.0 a)))))
(assert-true "the address in a child's scale is refused: that is not conjugate"
             (refused? (lambda () (gibbs-structure (stage (inscale NPTS)) :a))))

(assert-true "the address in a divisor is refused"
             (refused? (lambda () (ir-affine '(/ 1.0 (choice :a)) :a))))

;; The distinction the body table exists to draw, one level below the one
;; staging draws: a DECLARED function can be called by a kernel, and has
;; no body in this language, so whether the address enters it affinely
;; cannot be read at all. Refused where that becomes true.
(wgsl-declare! 'gibbs-opaque "erfc" '(:f32) :f32)
(define-gen (viadecl npts)
  (let ((a (at :a (normal 0 1.0))))
    (at :ys (batch-i npts (j) (normal (gibbs-opaque a) 1.0)))))
(assert-true "an address flowing through a declared-only helper is refused"
             (refused? (lambda () (gibbs-structure (stage (viadecl NPTS)) :a))))

;; But only when the address actually flows through it. A model may call
;; anything it likes on data, because an expression free of the address is
;; a constant for this update whatever it contains.
(define-gen (declonconst npts)
  (let ((a (at :a (normal 0 1.0))))
    (at :ys (batch-i npts (j) (normal (+ a (gibbs-opaque 0.5)) 1.0)))))
(assert-true "a declared helper applied to a constant does not block the read"
             (guard (e (#t #f))
                    (gibbs-structure (stage (declonconst NPTS)) :a)
                    #t))

(assert-true "an address the model does not have is refused"
             (refused? (lambda () (gibbs-structure st :nope))))
(assert-true "and a batched choice has no scalar update"
             (refused? (lambda () (gibbs-structure st :ys))))


;;--- the update, and a real oracle for it -------------------------------
;; The header above says a Gibbs update cannot be checked by comparing two
;; backends. That is true of a second DERIVATION, which would repeat the
;; same structural mistake — and it is not true of a measurement of the
;; model. When the conditional is Gaussian the log-joint is exactly
;; quadratic in the address, so three evaluations of staged-logpdf
;; determine it as an identity rather than a fit.
;;
;; So the claim divides, and each half is checked by what can see it:
;; gibbs-structure proves from the text THAT the conditional is Gaussian,
;; and the probe below measures WHICH Gaussian, sharing nothing with the
;; closed form but the model.

(define ch {:a 0.3 :b -0.1 :c 0.9 :ys ys})

(define (closed addr) (gibbs-posterior st (gibbs-structure st addr) ch))
(define (probed addr h) (gibbs-posterior-by-probe st addr ch h))

(define (agrees? addr)
  (let ((cf (closed addr)) (pr (probed addr 0.5)))
    (and (< (abs (- (car cf) (car pr))) 1e-11)
         (< (abs (- (cdr cf) (cdr pr))) 1e-11))))

(assert-true "the closed-form posterior for :a is the one the log-joint has"
             (agrees? :a))
(assert-true "and for :b, whose coefficient is the design"    (agrees? :b))
(assert-true "and for :c, whose coefficient is one"           (agrees? :c))

;; Step-size independence is what distinguishes an identity from a finite
;; difference. A quadratic is recovered exactly at ANY h, so a probe that
;; drifted with h would mean the conditional is not actually quadratic —
;; which is the assumption gibbs-structure is supposed to have proved.
(assert-true "the probe does not depend on its step size, so it is an identity"
             (let ((p1 (probed :a 0.01)) (p2 (probed :a 3.0)))
               (and (< (abs (- (car p1) (car p2))) 1e-9)
                    (< (abs (- (cdr p1) (cdr p2))) 1e-9))))

;; Data informs: eight observations must leave the address better known
;; than its prior did, and pull it off the prior mean.
(assert-true "the posterior is tighter than the prior"
             (< (cdr (closed :a)) 1.5))
(assert-true "and has moved off the prior mean"
             (> (abs (- (car (closed :a)) 0.0)) 1e-6))

;; The degenerate case the sum makes obvious: with no children the
;; precision is the prior's alone, so the update returns the prior. Worth
;; asserting because it is the identity a filter must satisfy before any
;; observation arrives.
(define-gen (lonely) (let ((a (at :a (normal 0.5 2.0)))) a))
(define lst (stage (lonely)))
(assert-true "an address with no children has its prior as its posterior"
             (let ((p (gibbs-posterior lst (gibbs-structure lst :a) {:a 0.0})))
               (and (< (abs (- (car p) 0.5)) 1e-12)
                    (< (abs (- (cdr p) 2.0)) 1e-12))))

(suite-summary)
