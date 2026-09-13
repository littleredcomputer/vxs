;;----------------------------------------------------------------------
;; Layer 24: staging — a model read rather than run
;;
;; lib/stage.scm turns a model's SOURCE into a description of its
;; log-joint, which two backends consume: one evaluates it on the VM, the
;; other emits kernel code. The load-bearing claim is that the reading did
;; not change the meaning, and the only way to know that is to compare the
;; staged answer against the one the coroutine produces.
;;
;; WHY THE COMPARISON IS EXACT RATHER THAN APPROXIMATE. It is tempting to
;; compare the staged path against the DEVICE and call agreement to f32
;; precision a pass. That conflates two unrelated risks: whether the
;; compiler read the model correctly (a structural question, exactly
;; reproducible) and whether f32 agrees with f64 (a numerical question
;; lib/dist.scm already characterises). Testing them together at 1e-6
;; means a structural bug small enough to hide under the tolerance passes.
;; So this layer tests the structural half alone, in f64, on both sides —
;; and it comes out bit-identical, because both paths sum the same terms
;; in the same order.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")
(load "lib/stage.scm")

(test-suite "24_stage: staging a model from its source")

;;--- the model ----------------------------------------------------------
;; Every dependency arrives as a PARAMETER. That is the tax: staging has
;; no eval and cannot read a global — which is also why a staged snapshot
;; cannot go stale, since there is no captured global left to change.

(define-dual (curve-elem (x :f32) (a :f32) (b :f32) (c :f32))
  (+ (* a x x) (* b x) c))

(define NPTS 10)

(define xs (bytes-view (make-bytes (* NPTS 8)) :f64))
(let loop ((i 0))
  (if (< i NPTS)
      (begin (view-set! xs i (+ -2.0 (* (/ 4.0 NPTS) i)))
             (loop (+ i 1)))))

(define ys (bytes-view (make-bytes (* NPTS 8)) :f64))
(rng-fill-normal! (rng-make 0 9 0) ys 0 NPTS 1.0 0.5)

(define-gen (curve xs sigma npts)
  (let* ((a (at :a (normal 0 1.5)))
         (b (at :b (normal 0 1.5)))
         (c (at :c (normal 0 1.5))))
    (at :ys (batch-i npts (j)
              (normal (curve-elem (view-ref xs j) a b c) sigma)))))

(define st (stage (curve xs 1.0 NPTS)))

;;--- what was read ------------------------------------------------------

(assert-equal "the choices are found, in source order, with their shapes"
              '((:a . scalar) (:b . scalar) (:c . scalar) (:ys batched 10))
              (:choices st))
(assert-equal "the buffers the model actually read are collected"
              '(xs) (map car (:buffers st)))
(assert-equal "there is one term per choice"
              4 (length (:terms st)))

;; The IR is a plain datum on purpose — a format, not an object — so this
;; asserts its literal shape. Something other than the reader above could
;; emit one and both backends would still consume it.
(assert-equal "a scalar choice becomes a score against its own value"
              '(score (normal 0 1.5) (choice :a))
              (car (:terms st)))
(assert-equal "and a batched one becomes a fold over the data it was given"
              '(sum-over 10 j
                 (score (normal (call curve-elem
                                      (data xs j) (choice :a) (choice :b) (choice :c))
                                1.0)
                        (choice-i :ys j)))
              (list-ref (:terms st) 3))

;;--- backend one: the VM, against assess --------------------------------
;; The whole point of the exercise.

(define (agree? a b c)
  (let ((ch {:a a :b b :c c :ys ys}))
    (abs (- (car (assess (curve xs 1.0 NPTS) ch))
            (staged-logpdf st ch)))))

(assert-true "the staged log-joint agrees with assess"      (< (agree? 2.0 -1.0 0.5) 1e-12))
(assert-true "at another particle"                          (< (agree? -0.7 3.1 -2.2) 1e-12))
(assert-true "at one far out in the tails"                  (< (agree? 9.0 -9.0 9.0) 1e-12))
(assert-true "and at the origin"                            (< (agree? 0.0 0.0 0.0) 1e-12))

;; Stronger than the tolerance above requires, and kept deliberately: the
;; two paths perform the same operations on the same values in the same
;; order, so the answer is not merely close, it is the same double. If
;; this ever weakens to "close", something reordered the sum, and that is
;; worth being told about even though it would still be correct.
(assert-equal "and does so bit-for-bit, because the term order is the source order"
              0.0 (agree? 2.0 -1.0 0.5))

;;--- backend two: the device --------------------------------------------
;; Emitted as a kernel-language expression rather than as text, so
;; lib/wgsl.scm checks it by exactly the rules it checks everything by.

(wgsl-declare! 'xs "xs_at" '(:u32) :f32)
(wgsl-declare! 'ys "ys_at" '(:u32) :f32)

;; A scalar choice is a plain name (a kernel parameter); a batched choice
;; and a data buffer are one-argument accessors. That is the interface a
;; wrangle has to supply, and the shape lib/wrangle.scm already declares
;; buffer readers in.
(define KENV '((a . :f32) (b . :f32) (c . :f32)))

(assert-equal "the emitted kernel expression type-checks to a scalar"
              :f32 (wgsl-type (staged-kernel st) KENV))

(set! wgsl-counter 0)
(define ksrc (wgsl-body (staged-kernel st) KENV ""))

(assert-true "the batched choice became the device's bounded fold"
             (string-contains? ksrc "for (var j_1 : u32 = 0u; j_1 < 10u;"))
(assert-true "the dual'd helper is called by its underscored name"
             (string-contains? ksrc "curve_elem(xs_at(j_1), a, b, c)"))
(assert-true "the scores are the declared device functions"
             (string-contains? ksrc "logpdf_normal(a, 0.0, 1.5)"))
(assert-true "and the fold accumulates rather than being summed after the fact"
             (string-contains? ksrc "acc_2 = (acc_2 + logpdf_normal(ys_at(j_1)"))

;;--- the model still runs -----------------------------------------------
;; Staging reads the source; it must not have disturbed what running it
;; does. batch-i is a distribution like any other on this path.

(define tr (sample (curve xs 1.0 NPTS) 4))
(assert-equal "the model still samples, and the batched address holds a view"
              10 (view-length (:retval (:ys tr))))
(assert-true "and its score is the sum over the batch, not one element"
             (< (abs (- (:score (:ys tr))
                        (let loop ((j 0) (acc 0.0))
                          (if (= j 10)
                              acc
                              (loop (+ j 1)
                                    (+ acc (logpdf-normal
                                            (view-ref (:retval (:ys tr)) j)
                                            (curve-elem (view-ref xs j)
                                                        (:retval (:a tr))
                                                        (:retval (:b tr))
                                                        (:retval (:c tr)))
                                            1.0)))))))
                1e-9))

;;--- what staging refuses, and by name ----------------------------------
;; A model that cannot stage is not broken — it stays on the fiber path,
;; which is the general case. What matters is that it is told so at the
;; offending construct rather than miscompiled into something plausible.

(define (refused? thunk) (guard (e (#t #t)) (thunk) #f))

(define-gen (uses-a-global) (at :x (normal SOME-GLOBAL 1.0)))
(assert-true "a global is refused: staging cannot read one, so it must be a parameter"
             (refused? (lambda () (stage (uses-a-global)))))

(define-gen (uses-a-stranger v) (at :x (normal (mystery-helper v) 1.0)))
(assert-true "a helper that never entered the kernel domain is refused"
             (refused? (lambda () (stage (uses-a-stranger 1.0)))))

(define-gen (uses-beta) (at :p (beta 2.0 3.0)))
(assert-true "a distribution with no device score is refused (beta needs lgamma)"
             (refused? (lambda () (stage (uses-beta)))))

(define-gen (uses-gamma) (at :g (gamma 2.0 1.0)))
(assert-true "and gamma, for the same reason"
             (refused? (lambda () (stage (uses-gamma)))))

;; Exponential is the counter-case, and the pair is the point: both are
;; families a model may name on the fiber path, and only one of them can
;; go to a device. That boundary is lib/stat.wgsl's, not a limit here —
;; and it moved once already, when writing the host score as a dual put
;; logpdf_exponential on a device that had a sampler and nothing to weight
;; it with.
(define-gen (waits rate) (at :t (exponential rate)))
(define exp-st (stage (waits 2.0)))
(assert-true "exponential stages, and agrees with assess"
             (< (abs (- (staged-logpdf exp-st {:t 0.75})
                        (car (assess (waits 2.0) {:t 0.75}))))
                1e-12))
(assert-equal "its kernel calls the device score by name"
              '(logpdf-exponential t 2.0) (staged-kernel exp-st))
;; No declaration needed for either name, and both absences say something.
;; logpdf-exponential is registered by its own define-dual; and a SCALAR
;; choice becomes a kernel parameter rather than an accessor, so `t` is
;; supplied by the environment, not called.
(assert-equal "and type-checks with the choice supplied as a parameter"
              :f32 (wgsl-type (staged-kernel exp-st) '((t . :f32))))

(define-gen (repeats-an-address)
  (let* ((p (at :x (normal 0 1))) (q (at :x (normal 0 1)))) (+ p q)))
(assert-true "the same address twice is refused"
             (refused? (lambda () (stage (repeats-an-address)))))

(define-gen (batches-by-a-computed-index xs n)
  (at :v (batch-i n (j) (normal (view-ref xs (+ j 1)) 1.0))))
(assert-true "a computed index is refused — that is a gather, a different primitive"
             (refused? (lambda () (stage (batches-by-a-computed-index xs 3)))))

(define-gen (adds-a-whole-batch n)
  (let ((v (at :v (batch-i n (j) (normal 0 1))))) (+ v 1.0)))
(assert-true "arithmetic on a batched choice is refused rather than meaning its first element"
             (refused? (lambda () (stage (adds-a-whole-batch 3)))))

(assert-true "a gf built by hand has no source to read, and says so"
             (refused? (lambda () (stage ((gf (lambda () 1)))))))

;; The distinction the dual table exists to draw: erfc is DECLARED for the
;; device out of lib/stat.wgsl, so a call to it type-checks in a kernel —
;; but it has no Scheme half registered, so the host backend cannot
;; evaluate it, and a model using it could never be checked against
;; assess. Refused at the point where that becomes true.
(wgsl-declare! 'erfc-device-only "erfc" '(:f32) :f32)
(define-gen (calls-a-declared-only v) (at :x (normal (erfc-device-only v) 1.0)))
(assert-true "staging accepts a declared function for the device"
             (guard (e (#t #f)) (stage (calls-a-declared-only 0.5)) #t))
(assert-true "but the host backend refuses it, having no Scheme half to call"
             (refused? (lambda ()
                         (staged-logpdf (stage (calls-a-declared-only 0.5))
                                        {:x 1.0}))))

;;--- strict f32: narrowing the oracle on purpose -------------------------
;; The two backends differ in precision by design, so comparing them is a
;; measurement rather than a test. `with-staged-f32` exists to separate the
;; two things a measurement cannot tell apart: whether a residual gap is
;; the accumulator's WIDTH or the two sides disagreeing about WHAT to
;; compute. It rounds where the IR names an operation, which is where the
;; accumulator lives.
;;
;; Note what it cannot reach, since it bounds every conclusion drawn from
;; it: a registered helper is a Scheme procedure, so curve-elem's two
;; multiplies and logpdf-normal's subexpressions still run in f64 and are
;; rounded only on the way out.

(assert-true "strict f32 is off unless asked for" (not staged-f32?))

(assert-true "and the form restores the flag"
             (begin (with-staged-f32 1) (not staged-f32?)))

;; unwind-protect, not a plain begin: a raising body must not leave the
;; oracle silently narrowed for everything that runs after it.
(assert-true "even when the body raises"
             (begin (guard (e (#t #f)) (with-staged-f32 (error 'stage "boom")))
                    (not staged-f32?)))

;; That the mode DOES something, stated on a value whose f32 form is not
;; its f64 form. Asserting that a log-joint moves would be brittle; this
;; is exact.
(assert-true "rounding is a real narrowing"
             (not (= 0.1 (with-staged-f32 (sf32 0.1)))))
(assert-true "and is idempotent, so nesting the form changes nothing"
             (= (with-staged-f32 (sf32 0.1))
                (with-staged-f32 (sf32 (with-staged-f32 (sf32 0.1))))))

;;--- blocked summation in the emitted fold ------------------------------
;; A likelihood fold adds many terms into a growing f32 total, so each
;; addend's low bits fall below the accumulator's ULP and the error grows
;; as O(n). Four partial sums in a rotating vec4 shorten the dependent
;; chain to n/4. It is a pure REASSOCIATION, which is the whole point:
;; compensated summation was tried, and the device's compiler deleted it
;; because (t - sum) - y is algebraically zero. There is nothing here for
;; an optimiser to cancel.

(assert-true "blocked is off unless asked for" (not staged-blocked?))
(assert-true "and the form restores the flag"
             (begin (with-staged-blocked 1) (not staged-blocked?)))

(assert-true "the plain fold starts from a scalar zero"
             (equal? 0.0 (caddr (list-ref (staged-kernel st) 4))))

(define kb (with-staged-blocked (staged-kernel st)))

(assert-equal "the blocked fold accumulates four lanes"
              '(vec4 0.0 0.0 0.0 0.0)
              (caddr (cadr (car (cadr (list-ref kb 4))))))

;; Still a scalar log-joint, so nothing downstream of the kernel can tell
;; which summation order it got.
(assert-equal "the blocked kernel still type-checks to a scalar"
              :f32 (wgsl-type kb KENV))
(assert-true "and it is a different kernel from the plain one"
             (not (equal? (staged-kernel st) kb)))

(define (f32gap a b c)
  (let ((ch {:a a :b b :c c :ys ys}))
    (abs (- (staged-logpdf st ch)
            (with-staged-f32 (staged-logpdf st ch))))))

;; Bounded, not zero: the claim is that the narrow evaluation is the same
;; computation at a smaller width, so it must stay within a few ulp of the
;; wide one rather than agree with it.
(assert-true "the f32 log-joint stays within a few ulp of the f64 one"
             (< (f32gap 2.0 -1.0 0.5) 1e-3))
(assert-true "at another particle"      (< (f32gap -0.7 3.1 -2.2) 1e-3))
(assert-true "and far out in the tails" (< (f32gap 9.0 -9.0 9.0) 1e-2))

(suite-summary)
