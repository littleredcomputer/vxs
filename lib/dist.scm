;;; Distributions on the host.
;;;
;;; A faithful port of lib/stat.wgsl. The two files must stay in step, so
;;; the ordering here follows the WGSL exactly and the constants are copied
;;; character for character rather than rederived — testcases/suite/22_dist.scm
;;; extracts them back out of the .wgsl and asserts they still agree.
;;;
;;; WHY A PORT RATHER THAN A SECOND DESIGN. Threefry is a pure function of
;;; (counter, key), so a host draw at a given counter is the same draw the
;;; device makes at that counter. That makes this an ORACLE for the shader:
;;; run the same model both places and the answers should agree. A port
;;; that improved on the algorithm would forfeit exactly that.
;;;
;;; HOW CLOSE IS "AGREE". The uniform stream is bit-identical — rng-unit!
;;; is m/2^23 for m the top 23 bits, which is exact in f32 and f64 alike.
;;; Everything downstream is not: the device evaluates these polynomials in
;;; f32 and the host in f64, so samples agree to about f32 precision and no
;;; further. A discrepancy larger than that is a bug; a discrepancy in the
;;; last few bits is arithmetic.
;;;
;;; The generator itself is native — see rng-make / rng-u32! / rng-unit! in
;;; src/vx_vm.cpp, and the note there for why only the core moved down.

(load "lib/threefry.scm")   ; the portable reference the native core is checked against
(load "lib/wgsl.scm")       ; for define-dual — see the log densities below

(define dist-pi 3.141592653589793)

;;--- the error function and its inverse ---------------------------------
;; From Press, Numerical Recipes 3ed: a low-order Chebyshev fit, accurate
;; to about 1.2e-7 everywhere — single precision, which is all the device
;; can use anyway.

;; The nine Chebyshev coefficients, innermost last, in the order they
;; appear in lib/stat.wgsl. A list evaluated by Horner rather than a
;; nine-deep nest of parentheses: the nest is what the WGSL has to write,
;; but it is unreadable, miscounts silently, and cannot be checked against
;; anything. Layer 22 reads these very numbers back out of the .wgsl.
(define erfc-coefficients
  '(1.00002368 0.37409196 0.09678418 -0.18628806 0.27886807
    -1.13520398 1.48851587 -0.82215223 0.17087277))

(define (erfc-poly t cs)
  ;; t*(c0 + t*(c1 + ... + t*c8)), which is what the nest spells out.
  (* t (let loop ((cs (reverse cs)) (acc 0.0))
         (if (null? cs) acc (loop (cdr cs) (+ (car cs) (* t acc)))))))

;; THE REFERENCE, not the implementation. `erfc`, `inv-erfc` and `inv-erf`
;; are native primitives — see vxs_erfc in src/vx_vm.cpp for why: every
;; normal draw is inverse-CDF, so this polynomial IS the cost of a normal,
;; and bulk normals ran 185x slower than bulk uniforms entirely because of
;; it. These transcriptions stay because they are readable beside
;; lib/stat.wgsl and because layer 22 asserts the native versions against
;; them — the same arrangement lib/threefry.scm has with the native core.
(define (erfc/reference x)
  (let* ((z   (abs x))
         (t   (/ 2.0 (+ 2.0 z)))
         (ans (* t (exp (+ (- (* z z)) -1.26551223 (erfc-poly t erfc-coefficients))))))
    (if (>= x 0.0) ans (- 2.0 ans))))

;; http://www.mimirgames.com/articles/programming/approximations-of-the-inverse-error-function/
;; One Newton refinement; the WGSL comments out a second, so this does too.
(define (inv-erfc/reference x)
  (let* ((pp (if (< x 1.0) x (- 2.0 x)))
         (t  (sqrt (* -2.0 (log (/ pp 2.0)))))
         (r0 (* -0.70711 (- (/ (+ 2.30753 (* t 0.27061))
                               (+ 1.0 (* t (+ 0.99229 (* t 0.04481)))))
                            t)))
         (er (- (erfc/reference r0) pp))
         (r  (+ r0 (/ er (- (* 1.12837916709551257 (exp (- (* r0 r0)))) (* r0 er))))))
    (if (> x 1.0) (- r) r)))

(define (inv-erf/reference x) (inv-erfc/reference (- 1.0 x)))

;;--- samplers -----------------------------------------------------------
;; Every one of these takes the generator explicitly. On the device it is
;; per-invocation private state and therefore implicit; here there may be
;; many streams alive at once, and hiding which one a draw came from is
;; precisely the confusion this whole design exists to avoid.
;;
;; ORDER OF CONSUMPTION IS PART OF THE CONTRACT. random-normal takes one
;; uniform, random-gamma takes two per rejection attempt. Change how many
;; a sampler draws and every downstream value shifts — which is why these
;; are ports and not reimplementations.

;; The ONLY place randomness enters. Everything else is built on it.
(define (random-uniform r low high)
  (let* ((a (rng-unit! r))
         (u (+ (* (- high low) a) low)))
    (max low u)))

(define (random-normal r loc scale)
  (let ((u (* (sqrt 2.0) (inv-erf (random-uniform r -1.0 1.0)))))
    (+ loc (* scale u))))

(define (random-exponential r lambda)
  (let ((u (- 1.0 (random-uniform r 0.0 1.0))))   ; u in (0, 1]
    (/ (- (log u)) lambda)))

(define (random-flip r prob) (< (random-uniform r 0.0 1.0) prob))

;; Marsaglia-Tsang, https://dl.acm.org/doi/pdf/10.1145/358407.358414
;;
;; Three attempts and then give up, matching the device — which cannot
;; loop unboundedly and has no cheap NaN to return. Giving up is rare and
;; silent, so it is COUNTED: dist-failures is the only evidence that a
;; sample was fabricated rather than drawn.
(define dist-failures 0)
(define (dist-reset-failures!) (set! dist-failures 0))

;; The squeeze itself, valid only for alpha >= 1. Below that d = alpha-1/3
;; goes non-positive, sqrt(9d) is the root of a non-positive number, the
;; acceptance test can never pass, and three attempts later a fabricated
;; 1.0 comes back — a perfectly plausible gamma value. Measured before the
;; boost below existed: alpha <= 1/3 ALWAYS fabricated, and alpha = 0.5
;; fabricated 8 times in 2000 because the acceptance rate degrades as
;; alpha approaches 1/3 from above.
(define (gamma-core r alpha)
  (let ((d (- alpha (/ 1.0 3.0))))
    (let loop ((i 0))
      (if (= i 3)
          (begin (set! dist-failures (+ dist-failures 1)) 1.0)
          (let* ((x  (random-normal r 0.0 1.0))
                 (u  (random-uniform r 0.0 1.0))
                 (v  (expt (+ 1.0 (/ x (sqrt (* 9.0 d)))) 3))
                 (dv (* d v)))
            (if (< (log u) (+ (* 0.5 (expt x 2)) d (- dv) (* d (log v))))
                dv
                (loop (+ i 1))))))))

;; Marsaglia and Tsang's own remedy for alpha < 1, from the same paper:
;;
;;   Gamma(alpha) = Gamma(alpha + 1) * U^(1/alpha)
;;
;; It does two things. It extends validity to every alpha > 0, and it
;; keeps the squeeze in the regime it is good at — so the fabrication rate
;; falls rather than merely stopping at zero.
;;
;; CONSUMPTION ORDER: the boost uniform is drawn AFTER the core's draws.
;; That is part of the contract, not an implementation detail — the device
;; must consume in the same order or the two stop agreeing.
(define (random-gamma-theta-one r alpha)
  (if (< alpha 1.0)
      (let* ((g (gamma-core r (+ alpha 1.0)))
             (u (random-uniform r 0.0 1.0)))
        (* g (expt u (/ 1.0 alpha))))
      (gamma-core r alpha)))

;;--- beta ---------------------------------------------------------------
;; X/(X+Y) with X ~ Gamma(alpha,1) and Y ~ Gamma(beta,1). Which is why the
;; boost above had to come first: Beta(0.5, 0.5) is Jeffreys' prior, and
;; without it both draws would have been fabricated 1.0 and every sample
;; would have been exactly 0.5.
(define (random-beta r alpha beta)
  (let* ((x (random-gamma-theta-one r alpha))
         (y (random-gamma-theta-one r beta)))
    (/ x (+ x y))))

;;--- log gamma, as a dual ------------------------------------------------
;; WGSL has no lgamma, which is what kept gamma and beta off the device.
;; Lanczos supplies one that is loop-free -- a fixed unrolled sum, which is
;; the whole reason it can be a dual body: there is no iteration in that
;; language and none needed here.
;;
;; g = 7, nine coefficients. MEASURED against the VM's std::lgamma: within
;; 5e-15 relative over z in [0.25, 500], which is f64-grade, so the host
;; half loses nothing by using it and the device half is as good as f32
;; allows. That is what makes one body honest for both -- had the
;; approximation been f32-grade, define-dual would have forced a worse
;; oracle on the host.
;;
;; NO REFLECTION, so the domain is z > 0. Every concentration, shape and
;; rate a prior carries is positive, and the series is accurate below 0.5
;; as well, so the usual (z < 0.5) branch buys nothing here.
(define-dual (lgamma-lanczos (z :f32))
  (let* ((w (- z 1.0))
         (x (+ 0.99999999999980993
               (/ 676.5203681218851     (+ w 1.0))
               (/ -1259.1392167224028   (+ w 2.0))
               (/ 771.32342877765313    (+ w 3.0))
               (/ -176.61502916214059   (+ w 4.0))
               (/ 12.507343278686905    (+ w 5.0))
               (/ -0.13857109526572012  (+ w 6.0))
               (/ 9.9843695780195716e-6 (+ w 7.0))
               (/ 1.5056327351493116e-7 (+ w 8.0))))
         (t (+ w 7.5)))
    (- (+ 0.9189385332046727 (* (+ w 0.5) (log t)) (log x)) t)))

(define-dual (lbeta (a :f32) (b :f32))
  (- (+ (lgamma-lanczos a) (lgamma-lanczos b)) (lgamma-lanczos (+ a b))))

;; (alpha-1)log(v) + (beta-1)log(1-v) - lbeta(alpha,beta).
;;
;; -inf outside [0,1] AND at the endpoints. For alpha < 1 the true density
;; diverges at 0 rather than vanishing, so -inf there is wrong in
;; principle — but the endpoints have measure zero and the sampler cannot
;; produce them, so this is the boundary convention rather than a claim
;; about the density.
;;
;; THE GUARD IS A VALUE, not a branch around the arithmetic, which is
;; logpdf-uniform's idiom and is forced by the device: `if` lowers to
;; select, so both arms are evaluated and an arm cannot protect the other.
;; `safe` keeps every log's argument in range and `(log ind)` contributes
;; 0 inside the support and -inf outside. Branching instead would compute
;; (log v) at v <= 0 -- nan, not -inf -- and multiplying it by (alpha - 1)
;; at alpha = 1 gives 0 * nan, which is nan rather than the -inf wanted.
(define-dual (logpdf-beta (v :f32) (alpha :f32) (beta :f32))
  (let* ((inside? (and (> v 0.0) (< v 1.0)))
         (safe (if inside? v 0.5))
         (ind  (if inside? 1.0 0.0)))
    (+ (* (- alpha 1.0) (log safe))
       (* (- beta 1.0) (log (- 1.0 safe)))
       (- 0.0 (lbeta alpha beta))
       (log ind))))

(define (random-gamma r alpha lambda)
  (* (/ 1.0 lambda) (random-gamma-theta-one r alpha)))

;;--- log densities ------------------------------------------------------
;; The half that turns samples into weights, and the half that is now
;; WRITTEN ONCE. These used to exist twice — here in Scheme and again as
;; hand-written WGSL in lib/stat.wgsl — kept in step by transcribing one
;; into the other and asserting afterwards that they still agreed. The
;; de-compiled shape below, odd intermediate names and all, is a fossil of
;; that arrangement: it was written to be read side by side with the WGSL.
;;
;; define-dual removes the second copy rather than checking it. The body
;; is spliced unquoted for the VM and quoted for the kernel compiler, so
;; there is no longer a transcription that could drift, and lib/stat.wgsl
;; no longer defines these at all.
;;
;; WHAT STAYS BEHIND, and why it is not pending work. The SAMPLERS cannot
;; follow: every one of them takes the generator explicitly here and finds
;; it as per-invocation private state on the device, so the two genuinely
;; have different signatures. logpdf-gamma and logpdf-beta cannot either —
;; they need lgamma, which WGSL does not have. Those absences are exactly
;; what lib/stage.scm refuses a model for, by name.

(define-dual (logpdf-normal (v :f32) (loc :f32) (scale :f32))
  (let* ((d (/ v scale))
         (e (/ loc scale))
         (f (- d e))
         (h (* -0.5 (expt f 2.0)))
         (k (+ 0.9189385175704956 (log scale))))
    (- h k)))

(define-dual (logpdf-flip (v :f32) (p :f32))
  (let* ((h (log (+ (- p) 1.0)))      ; log1p(-p)
         (i (log p))
         (k (- 1.0 v))
         (o (if (= k 0.0) 0.0 (* h k)))
         (s (if (= i 0.0) 0.0 (* i v))))
    (+ o s)))

;; log(lambda) - lambda*v on the support, and -inf below it. Absent from
;; lib/stat.wgsl, which has random_exponential and no score for it — added
;; here first because the host is where scoring happens; the device wants
;; the same function whenever a kernel needs to weight one.
;; The parameter is `rate` rather than `lambda` now that this is a dual:
;; the name reaches the emitted WGSL, and shadowing a special form to
;; produce it was never a good trade for one word.
(define-dual (logpdf-exponential (v :f32) (rate :f32))
  ;; -inf below the support, not a large negative: that is what log(0) is,
  ;; it is what every other logpdf here returns off-support, and it behaves
  ;; correctly downstream — (exp -inf) is 0, so an impossible value gets
  ;; exactly zero probability rather than an extremely small one.
  ;;
  ;; On the device `if` is select, so BOTH arms are evaluated — which is
  ;; safe here only because neither traps.
  ;;
  ;; `-inf` is a literal the reader already understands, and lib/wgsl.scm
  ;; stops it from being one on the way out: WGSL has no spelling for an
  ;; infinity, so the emitter turns it into a call to a helper that
  ;; divides by a runtime zero. Writing (log 0.0) here instead is correct
  ;; Scheme and a shader that will not compile, which is exactly what this
  ;; line used to say.
  (if (< v 0.0) -inf (- (log rate) (* rate v))))

;; Gamma(alpha, rate=lambda). lambda is the RATE, matching random-gamma,
;; which multiplies a Gamma(alpha, theta=1) draw by 1/lambda.
;;
;;   alpha*log(lambda) - lgamma(alpha) + (alpha-1)*log(v) - lambda*v
;;
;; It used to say this would stay absent until someone wrote an lgamma for
;; WGSL, and that the shader version would then be an approximation to be
;; checked against this one. lgamma-lanczos above is that lgamma, and
;; define-dual means there is no second version to check: one body, and the
;; thing it is measured against is the VM's std::lgamma rather than a
;; parallel implementation that could drift.
;;
;; The support guard is a value rather than a branch, for the reason given
;; on logpdf-beta.
(define-dual (logpdf-gamma (v :f32) (alpha :f32) (lambda :f32))
  (let* ((pos? (> v 0.0))
         (safe (if pos? v 1.0))
         (ind  (if pos? 1.0 0.0)))
    (+ (* alpha (log lambda))
       (- 0.0 (lgamma-lanczos alpha))
       (* (- alpha 1.0) (log safe))
       (- 0.0 (* lambda safe))
       (log ind))))

;; The NaN branch is not decoration, and it is what the two copies
;; DISAGREED about: lib/stat.wgsl tested it and this file did not. Without
;; it a NaN compares false against both bounds, so it is judged inside the
;; support and scored as though it were an ordinary value — a finite
;; log-density for a number that is not one. Reconciling the two copies
;; meant picking the correct one, so the host gains the check.
(define-dual (logpdf-uniform (v :f32) (low :f32) (high :f32))
  (let* ((nan? (not (= v v)))
         (outside? (or (< v low) (> v high)))
         (l (if outside? 0.0 (/ 1.0 (- high low))))
         (q (if nan? v l)))
    (log q)))

;;--- bulk draws ---------------------------------------------------------
;; Filling a typed buffer rather than building a list, because a list of a
;; million draws is a million conses the collector must walk on every
;; cycle, while a bytes buffer is opaque to it. Measured: a 1M-element
;; vector costs 262us per collection, the equivalent buffer 2.75us.
;;
;; Both fills are native and do the whole loop in C++ — no per-draw
;; dispatch, no per-draw allocation. fill-normal! consumes exactly one
;; uniform per draw and transforms it exactly as random_normal does on the
;; device, because deviating from that order would forfeit the
;; correspondence this file exists to preserve.

;; EVERY sampler has a fill, and most of them are derived rather than
;; written. A fill is the same loop each time — draw, store, repeat — so
;; only the ones measurement says are hot get a native version, and the
;; rest cost nothing to have.
;;
;; That is also the shape a distribution object wants: supply `sample` and
;; `score`, get `fill` for free, override it when there is a reason.
(define (generic-fill sample)
  (lambda (r view start count . args)
    (let loop ((i 0))
      (if (< i count)
          (begin (view-set! view (+ start i) (apply sample r args))
                 (loop (+ i 1)))))))

;; Native, because these are the hot ones. Each consumes exactly ONE
;; uniform per element and transforms it exactly as its scalar sampler
;; does — a cheaper transform would break the correspondence with the
;; device that this whole file exists to keep.
(define (fill-uniform! r view start count low high)
  (rng-fill-uniform! r view start count low high))
(define (fill-normal! r view start count loc scale)
  (rng-fill-normal! r view start count loc scale))
(define (fill-flip! r view start count p)
  (rng-fill-flip! r view start count p))

;; Derived. Gamma's rejection loop consumes a variable number of uniforms,
;; so a native version would have to reproduce that exactly for no gain —
;; it is the sampler least likely to be filled in bulk.
(define fill-beta!        (generic-fill random-beta))
(define fill-exponential! (generic-fill random-exponential))
(define fill-gamma!       (generic-fill random-gamma))

;; The summed scores. Every scored distribution now has one; each hoists
;; whatever is constant across the buffer out of the loop, which is the
;; one arrangement a scalar version cannot make.
(define (sum-uniform! view start count low high)
  (logpdf-sum-uniform view start count low high))
(define (sum-normal! view start count loc scale)
  (logpdf-sum-normal view start count loc scale))
(define (sum-flip! view start count p)
  (logpdf-sum-flip view start count p))
(define (sum-beta! view start count alpha beta)
  (logpdf-sum-beta view start count alpha beta))
(define (sum-exponential! view start count lambda)
  (logpdf-sum-exponential view start count lambda))
(define (sum-gamma! view start count alpha lambda)
  (logpdf-sum-gamma view start count alpha lambda))

;; The raw generator operation, kept distinct: rng-fill-unit! is (0,1) and
;; belongs to the RNG layer, while fill-uniform! takes the bounds its
;; scalar sampler takes and belongs to the distribution layer. Conflating
;; them is what made fill-uniform! silently ignore low and high.
(define (fill-unit! r view start count) (rng-fill-unit! r view start count))

;;=======================================================================
;;  Distributions that are NOT ports
;;=======================================================================
;; Everything above mirrors lib/stat.wgsl, in its order, because that
;; correspondence is what makes this file an oracle. These four were
;; written here first, so there is no WGSL order to follow — and they are
;; grouped by DISTRIBUTION rather than by capability, so each one's
;; sampler, score, fill and sum read together. The ported section keeps
;; its own arrangement precisely because it has a second file to agree
;; with.
;;
;; Two of the four reach the device and two do not, and the split is the
;; same one as everywhere else here: laplace and cauchy have scores that
;; are ordinary arithmetic, so they are duals and lib/stage.scm lists
;; them. categorical needs a buffer read and dirichlet needs lgamma, so
;; they stay on the fiber path and a model using them is refused by name.

;;--- laplace ------------------------------------------------------------
;; A sign times an Exponential: two uniforms, side first, then magnitude.
;;
;; Not the textbook inverse CDF, which is
;; loc - b*sgn(u-1/2)*ln(1-2|u-1/2|) and diverges at BOTH ends of the unit
;; interval. rng-unit! attains one of them, so the usual answer is to clamp
;; u away from the ends — which alters the tails in the exact region a
;; heavy-tailed distribution exists to get right. Composing two samplers
;; that already handle their own endpoints fabricates nothing instead.
(define (random-laplace r loc scale)
  (let* ((neg (random-flip r 0.5))
         (mag (random-exponential r (/ 1.0 scale))))
    (if neg (- loc mag) (+ loc mag))))

(define-dual (logpdf-laplace (v :f32) (loc :f32) (scale :f32))
  (- (- (abs (/ (- v loc) scale))) (log (* 2.0 scale))))

(define fill-laplace! (generic-fill random-laplace))

(define (logpdf-sum-laplace view start count loc scale)
  ;; log(2b) is constant across the buffer, so it is subtracted once
  ;; rather than count times — the arrangement a scalar score cannot make.
  (let ((k (log (* 2.0 scale))))
    (let loop ((i 0) (acc 0.0))
      (if (= i count)
          (- acc (* count k))
          (loop (+ i 1)
                (- acc (abs (/ (- (view-ref view (+ start i)) loc) scale))))))))

(define (sum-laplace! view start count loc scale)
  (logpdf-sum-laplace view start count loc scale))

;;--- cauchy -------------------------------------------------------------
;; One uniform, and the textbook inverse CDF. It does not need laplace's
;; care because the divergence is not one: tan walks to a very large
;; number at the ends rather than to an infinity, since pi/2 is not
;; representable, and a Cauchy's support is the whole line anyway. A huge
;; sample from the tail of a Cauchy is a correct sample.
(define (random-cauchy r loc scale)
  (+ loc (* scale (tan (* dist-pi (- (random-uniform r 0.0 1.0) 0.5))))))

(define-dual (logpdf-cauchy (v :f32) (loc :f32) (scale :f32))
  (let ((z (/ (- v loc) scale)))
    (- (- (log (* 3.141592653589793 scale))) (log (+ 1.0 (* z z))))))

(define fill-cauchy! (generic-fill random-cauchy))

(define (logpdf-sum-cauchy view start count loc scale)
  (let ((k (log (* dist-pi scale))))
    (let loop ((i 0) (acc 0.0))
      (if (= i count)
          (- acc (* count k))
          (let ((z (/ (- (view-ref view (+ start i)) loc) scale)))
            (loop (+ i 1) (- acc (log (+ 1.0 (* z z))))))))))

(define (sum-cauchy! view start count loc scale)
  (logpdf-sum-cauchy view start count loc scale))

;;--- categorical --------------------------------------------------------
;; The parameter is a VIEW of weights, which makes this the first
;; distribution here whose parameter is not a scalar. The weights are
;; LINEAR and need not be normalised — rng-categorical! computes the total
;; itself, and the score below divides by it — so a caller may pass the
;; same buffer it is using for something else, and a normalising pass that
;; exists only to satisfy the sampler never happens.
;;
;; The VALUE is an index, carried as an ordinary number because that is
;; what a view holds. Exact to 2^24 in f32 and 2^53 in f64, which is well
;; past any plausible category count.
(define (random-categorical r ws)
  (rng-categorical! r ws 0 (view-length ws)))

(define (view-sum ws)
  (let loop ((i 0) (acc 0.0))
    (if (= i (view-length ws)) acc (loop (+ i 1) (+ acc (view-ref ws i))))))

;; Not a dual: reading a weight needs an indexed buffer read, which is the
;; gather lib/stage.scm refuses, so there is no device half to write yet.
(define (logpdf-categorical v ws)
  (let ((k (view-length ws))
        (i (inexact->exact (round v))))
    ;; Off-support is -inf, as everywhere else here: an index outside the
    ;; table, or one whose weight is zero, is impossible rather than
    ;; merely unlikely.
    (if (or (< i 0) (>= i k))
        -inf
        (let ((w (view-ref ws i)))
          (if (<= w 0.0)
              -inf
              (- (log w) (log (view-sum ws))))))))

(define fill-categorical! (generic-fill random-categorical))

(define (logpdf-sum-categorical view start count ws)
  ;; The total is one scan over the weights, hoisted out of the loop over
  ;; the buffer — which is the whole reason a summed score exists.
  (let ((tot (log (view-sum ws)))
        (k   (view-length ws)))
    (let loop ((i 0) (acc 0.0))
      (if (= i count)
          acc
          (let ((j (inexact->exact (round (view-ref view (+ start i))))))
            (if (or (< j 0) (>= j k))
                -inf
                (let ((w (view-ref ws j)))
                  (if (<= w 0.0)
                      -inf
                      (loop (+ i 1) (+ acc (- (log w) tot)))))))))))

(define (sum-categorical! view start count ws)
  (logpdf-sum-categorical view start count ws))

;;--- dirichlet ----------------------------------------------------------
;; The first distribution here whose VALUE is a vector rather than a
;; number. That needs no new machinery: `batch` already sits at an address
;; with a view for a value, and the record already has `unsupported` for
;; the capabilities such a distribution does not have. A Dirichlet is not
;; n independent draws, so it is not a batch — but it has the same shape
;; at the address, which is what matters to everything downstream.
;;
;; f64 rather than the f32 `batch` allocates. A batched choice is a
;; candidate GPU column and matches the device's width on purpose; a
;; Dirichlet has no device score at all, so there is no column to agree
;; with, and the components of a simplex can be small enough that the
;; logarithm below wants the precision.
;;
;; CONSUMES a gamma draw per component, in index order, each of which is a
;; rejection loop of variable length. So the stream position after a
;; Dirichlet is not a function of K alone — noted because order of
;; consumption is part of the contract everywhere else in this file.
(define (random-dirichlet r alpha)
  (let* ((k (view-length alpha))
         (out (bytes-view (make-bytes (* k 8)) :f64)))
    (let loop ((i 0) (tot 0.0))
      (if (= i k)
          (let norm ((j 0))
            (if (= j k)
                out
                (begin (view-set! out j (/ (view-ref out j) tot))
                       (norm (+ j 1)))))
          (let ((g (random-gamma r (view-ref alpha i) 1.0)))
            (view-set! out i g)
            (loop (+ i 1) (+ tot g)))))))

;; lgamma(sum a) - sum lgamma(a_i) + sum (a_i - 1) log(v_i)
(define (logpdf-dirichlet v alpha)
  (let ((k (view-length alpha)))
    (if (not (= (view-length v) k))
        (error 'distribution "dirichlet: the value and the concentration differ in length"))
    (let loop ((i 0) (asum 0.0) (lg 0.0) (acc 0.0))
      (if (= i k)
          (+ (- (lgamma asum) lg) acc)
          (let ((a (view-ref alpha i))
                (x (view-ref v i)))
            (if (<= x 0.0)
                -inf
                (loop (+ i 1)
                      (+ asum a)
                      (+ lg (lgamma a))
                      (+ acc (* (- a 1.0) (log x))))))))))
