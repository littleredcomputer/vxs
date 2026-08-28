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

(define (erfc x)
  (let* ((z   (abs x))
         (t   (/ 2.0 (+ 2.0 z)))
         (ans (* t (exp (+ (- (* z z)) -1.26551223 (erfc-poly t erfc-coefficients))))))
    (if (>= x 0.0) ans (- 2.0 ans))))

;; http://www.mimirgames.com/articles/programming/approximations-of-the-inverse-error-function/
;; One Newton refinement; the WGSL comments out a second, so this does too.
(define (inv-erfc x)
  (let* ((pp (if (< x 1.0) x (- 2.0 x)))
         (t  (sqrt (* -2.0 (log (/ pp 2.0)))))
         (r0 (* -0.70711 (- (/ (+ 2.30753 (* t 0.27061))
                               (+ 1.0 (* t (+ 0.99229 (* t 0.04481)))))
                            t)))
         (er (- (erfc r0) pp))
         (r  (+ r0 (/ er (- (* 1.12837916709551257 (exp (- (* r0 r0)))) (* r0 er))))))
    (if (> x 1.0) (- r) r)))

(define (inv-erf x) (inv-erfc (- 1.0 x)))

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

(define (flip r prob) (< (random-uniform r 0.0 1.0) prob))

;; Marsaglia-Tsang, https://dl.acm.org/doi/pdf/10.1145/358407.358414
;;
;; Three attempts and then give up, matching the device — which cannot
;; loop unboundedly and has no cheap NaN to return. Giving up is rare and
;; silent, so it is COUNTED: dist-failures is the only evidence that a
;; sample was fabricated rather than drawn.
(define dist-failures 0)
(define (dist-reset-failures!) (set! dist-failures 0))

(define (random-gamma-theta-one r alpha)
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

(define (random-gamma r alpha lambda)
  (* (/ 1.0 lambda) (random-gamma-theta-one r alpha)))

;;--- log densities ------------------------------------------------------
;; The half that turns samples into weights. Kept in the same de-compiled
;; shape as the WGSL, odd-looking intermediate names and all, because the
;; point is that the two can be read side by side.

(define (logpdf-normal v loc scale)
  (let* ((d (/ v scale))
         (e (/ loc scale))
         (f (- d e))
         (h (* -0.5 (expt f 2.0)))
         (k (+ 0.9189385175704956 (log scale))))
    (- h k)))

(define (logpdf-flip v p)
  (let* ((h (log (+ (- p) 1.0)))      ; log1p(-p)
         (i (log p))
         (k (- 1.0 v))
         (o (if (= k 0.0) 0.0 (* h k)))
         (s (if (= i 0.0) 0.0 (* i v))))
    (+ o s)))

(define (logpdf-uniform v low high)
  (let* ((outside? (or (< v low) (> v high)))
         (l (if outside? 0.0 (/ 1.0 (- high low)))))
    (log l)))

;;--- bulk draws ---------------------------------------------------------
;; Filling a typed buffer rather than building a list, because a list of a
;; million draws is a million conses the collector must walk on every
;; cycle, while a bytes buffer is opaque to it. Measured: a 1M-element
;; vector costs 262us per collection, the equivalent buffer 2.75us.
;;
;; rng-fill-unit! is native and fills in one call — 185M draws/second
;; against 9.9M for a Scheme loop calling rng-unit!. Anything that is just
;; uniforms should use it. The transformed samplers below are a Scheme
;; loop by necessity, and land around 1M/second, which is the honest cost
;; of erfc in bytecode.

(define (fill-uniform! r view start count) (rng-fill-unit! r view start count))

(define (fill-normal! r view start count loc scale)
  (let loop ((i 0))
    (if (< i count)
        (begin (view-set! view (+ start i) (random-normal r loc scale))
               (loop (+ i 1))))))
