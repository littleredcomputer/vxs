;;----------------------------------------------------------------------
;; Layer 22: distributions on the host (lib/dist.scm) and the native
;; Threefry core behind them.
;;
;; These exist so the host can draw what the device draws. That is worth
;; more than speed: run a model both places and the answers should agree,
;; which makes the CPU path an ORACLE for the shader rather than a second
;; opinion. Everything asserted here defends that correspondence.
;;
;; THREE IMPLEMENTATIONS OF ONE ALGORITHM live in this repo — the portable
;; Scheme reference (lib/threefry.scm), the device (lib/rng.wgsl), and the
;; native core (src/vx_vm.cpp), which exists because measurement said the
;; round loop was the cost. Two of the three can be run here and compared
;; directly. The third cannot, so it is defended differently: the shared
;; CONSTANTS are read back out of the .wgsl text and asserted against the
;; Scheme, which catches the failure that would otherwise be silent and
;; only visible as a slow statistical divergence.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")
(load "lib/dist.scm")

(test-suite "22_dist: host distributions and the native RNG core")

;;--- the native core against the portable reference ---------------------
;; Layer 13 pins lib/threefry.scm against the nine published known-answer
;; vectors. Pinning the native core against lib/threefry.scm therefore
;; pins it against those too, without repeating them.

(define (native-block ptnum seed stream)
  (let ((r (rng-make ptnum seed stream)))
    (list (rng-u32! r) (rng-u32! r) (rng-u32! r) (rng-u32! r))))

(define (reference-block ptnum seed stream)
  (let ((w (threefry4x32 (vector ptnum 0 stream 0) (vector seed 0 0 0))))
    (list (vector-ref w 0) (vector-ref w 1) (vector-ref w 2) (vector-ref w 3))))

(assert-equal "the native core matches the Scheme reference"
              (reference-block 0 12345 0) (native-block 0 12345 0))
(assert-equal "and at another point"
              (reference-block 7 12345 0) (native-block 7 12345 0))
(assert-equal "and on another stream"
              (reference-block 7 12345 3) (native-block 7 12345 3))
(assert-equal "and with another seed"
              (reference-block 7 999 3) (native-block 7 999 3))

;; ptnum, seed and stream must each move the draw independently. If any of
;; them were dropped on the floor — easy, since all three land in one
;; buffer — every point in a wrangle would share randomness and the
;; picture would still look plausible.
(assert-equal "ptnum separates points"
              #f (equal? (native-block 0 1 0) (native-block 1 1 0)))
(assert-equal "seed separates runs"
              #f (equal? (native-block 0 1 0) (native-block 0 2 0)))
(assert-equal "stream separates uses within a point"
              #f (equal? (native-block 0 1 0) (native-block 0 1 1)))

;; A block is four words and the counter advances on refill, so the fifth
;; draw must be the SECOND block's first word — not a repeat of the first.
(define r5 (rng-make 0 12345 0))
(rng-u32! r5) (rng-u32! r5) (rng-u32! r5) (rng-u32! r5)
(assert-equal "the fifth draw comes from the next block, via ctr.y"
              (vector-ref (threefry4x32 (vector 0 1 0 0) (vector 12345 0 0 0)) 0)
              (rng-u32! r5))

;;--- rng-unit! is bit-identical to the device ---------------------------
;; The ONE value that genuinely is bit-identical, and the reason is worth
;; keeping: lib/rng.wgsl builds a float by pasting the top 23 bits under a
;; fixed exponent and subtracting one, which is arithmetically m/2^23 —
;; exact in f32 and f64 alike. A division by 2^32 (which lib/threefry.scm's
;; u32->unit does, for its own purposes) would be a DIFFERENT number and
;; the two sides would silently disagree.

(define (top23 w) (/ (floor (/ w 512)) 8388608.0))
(define ru (rng-make 0 12345 0))
(define refw (threefry4x32 (vector 0 0 0 0) (vector 12345 0 0 0)))
(assert-equal "rng-unit! is m/2^23, not w/2^32"
              (top23 (vector-ref refw 0)) (rng-unit! ru))
(assert-equal "and again on the second word"
              (top23 (vector-ref refw 1)) (rng-unit! ru))

;; Never exactly zero. The 512 smallest words out of 2^32 would otherwise
;; produce 0.0, which becomes random-uniform(-1,1) = -1, inv-erf(-1),
;; log(0), and finally NaN out of random-normal — an element poisoned with
;; no diagnostic at all. See the note in lib/rng.wgsl.
;; The clamp cannot be provoked here — it needs one of the 512 smallest
;; words out of 2^32 — so it is defended the same way the device
;; implementation is: by pinning the constant in both texts. The native
;; core in src/vx_vm.cpp uses this same literal.
(assert-true "the clamp constant is the one lib/rng.wgsl uses"
             (string-contains? (embedded-source "rng.wgsl") "5.9604645e-8"))
(assert-equal "and no ordinary draw ever lands on zero"
              #t (let ((r (rng-make 0 271828 0)))
                   (let loop ((i 0))
                     (cond ((= i 4096) #t)
                           ((= (rng-unit! r) 0.0) #f)
                           (else (loop (+ i 1)))))))

;;--- bulk fill agrees with drawing one at a time ------------------------
;; The native fill exists for speed (185M draws/second against 9.9M), and
;; a fast path that produced different numbers would be worse than useless.

(define bulk-bytes (make-bytes (* 64 4)))
(define bulk (bytes-view bulk-bytes :f32))
(rng-fill-unit! (rng-make 0 4242 0) bulk 0 64)
(define one-at-a-time
  (let ((r (rng-make 0 4242 0)))
    (let loop ((i 0) (acc '()))
      (if (= i 64) (reverse acc) (loop (+ i 1) (cons (rng-unit! r) acc))))))
(assert-equal "bulk fill matches drawing one at a time"
              #t (let loop ((i 0) (xs one-at-a-time))
                   (cond ((= i 64) #t)
                         ;; the view is :f32, so compare at f32 precision
                         ((> (abs (- (view-ref bulk i) (car xs))) 1e-7) #f)
                         (else (loop (+ i 1) (cdr xs))))))

;;--- the constants must not drift from lib/stat.wgsl --------------------
;; The device implementation cannot be RUN here, so it is defended by
;; reading its text. Every number below appears in both files; if someone
;; edits one, this fails rather than the two quietly disagreeing in the
;; seventh decimal place for months.

(define wgsl-stat (embedded-source "stat.wgsl"))

(define (wgsl-has? s) (string-contains? wgsl-stat s))

(assert-equal "every erfc coefficient appears in stat.wgsl"
              '() (let loop ((cs erfc-coefficients) (missing '()))
                    (cond ((null? cs) (reverse missing))
                          ((wgsl-has? (number->string (car cs)))
                           (loop (cdr cs) missing))
                          (else (loop (cdr cs) (cons (car cs) missing))))))

(assert-true "the erfc leading constant matches"  (wgsl-has? "-1.26551223"))
(assert-true "inv_erfc's rational coefficients match"
             (and (wgsl-has? "-0.70711") (wgsl-has? "2.30753") (wgsl-has? "0.27061")
                  (wgsl-has? "0.99229") (wgsl-has? "0.04481")))
(assert-true "and its Newton constant"  (wgsl-has? "1.12837916709551257"))
(assert-true "logpdf_normal's half-log-2pi matches"
             (wgsl-has? "0.9189385175704956"))

;; Structural, not numeric: the samplers must keep drawing what they draw
;; now. random_normal takes ONE uniform; a change to Box-Muller would take
;; two and shift every downstream value in every kernel.
(assert-true "random_normal is still one inverse-CDF draw"
             (wgsl-has? "sqrt(2.0) * inv_erf(random_uniform(-1.0, 1.0))"))
(assert-true "random_gamma still gives up after three attempts"
             (wgsl-has? "i < 3u"))

;;--- consumption order --------------------------------------------------
;; How MANY uniforms a sampler takes is part of the contract: it decides
;; where every later draw lands. This is invisible in any single value and
;; is exactly the kind of thing a refactor breaks silently.

;; Run the sampler, then ask where in a fresh generator's sequence the
;; NEXT draw falls. That index is how many the sampler consumed.
(define (draws-taken thunk)
  (let ((reference (let ((s (rng-make 0 555 0)))
                     (let loop ((i 0) (acc '()))
                       (if (= i 16) (reverse acc)
                           (loop (+ i 1) (cons (rng-unit! s) acc))))))
        (r (rng-make 0 555 0)))
    (thunk r)
    (let ((next (rng-unit! r)))
      (let loop ((i 0) (xs reference))
        (cond ((null? xs) 'not-found)
              ((= (car xs) next) i)
              (else (loop (+ i 1) (cdr xs))))))))

(assert-equal "random-uniform takes exactly one draw"
              1 (draws-taken (lambda (r) (random-uniform r 0.0 1.0))))
(assert-equal "random-normal takes exactly one — inverse CDF, not Box-Muller"
              1 (draws-taken (lambda (r) (random-normal r 0.0 1.0))))
(assert-equal "random-exponential takes exactly one"
              1 (draws-taken (lambda (r) (random-exponential r 1.0))))
(assert-equal "flip takes exactly one"
              1 (draws-taken (lambda (r) (flip r 0.5))))

;;--- the samplers themselves --------------------------------------------

(assert-equal "erfc(0) is 1 to the approximation's accuracy"
              #t (< (abs (- (erfc 0.0) 1.0)) 2e-7))
(assert-equal "erfc(1) matches the true value"
              #t (< (abs (- (erfc 1.0) 0.15729920705028513)) 2e-7))
(assert-equal "erfc is symmetric about 1"
              #t (< (abs (- (+ (erfc 0.7) (erfc -0.7)) 2.0)) 2e-7))
(assert-equal "inv-erf inverts erf near zero"
              #t (< (abs (inv-erf 0.0)) 1e-6))

(assert-equal "logpdf-normal at the mode is -log(sqrt(2pi))"
              #t (< (abs (- (logpdf-normal 0.0 0.0 1.0) -0.9189385175704956)) 1e-9))
(assert-equal "logpdf-normal falls by 1/2 at one sigma"
              #t (< (abs (- (- (logpdf-normal 0.0 0.0 1.0) (logpdf-normal 1.0 0.0 1.0)) 0.5))
                    1e-9))
(assert-equal "logpdf-uniform is -log(width) inside the support"
              #t (< (abs (- (logpdf-uniform 0.5 0.0 2.0) (- (log 2.0)))) 1e-12))

;; Statistical sanity. Loose bounds deliberately: this is checking that the
;; sampler is not grossly wrong, not re-testing the approximation's
;; accuracy, which the erfc assertions above already pin.
(define (moments n draw)
  (let ((r (rng-make 0 8675309 0)))
    (let loop ((i 0) (s 0.0) (s2 0.0))
      (if (= i n)
          (let ((m (/ s n))) (cons m (- (/ s2 n) (* m m))))
          (let ((x (draw r))) (loop (+ i 1) (+ s x) (+ s2 (* x x))))))))

(define nm (moments 20000 (lambda (r) (random-normal r 0.0 1.0))))
(assert-equal "20000 standard normals have mean near zero"
              #t (< (abs (car nm)) 0.03))
(assert-equal "and variance near one"
              #t (< (abs (- (cdr nm) 1.0)) 0.05))

(define um (moments 20000 (lambda (r) (random-uniform r 0.0 1.0))))
(assert-equal "uniforms on (0,1) have mean near a half"
              #t (< (abs (- (car um) 0.5)) 0.01))
(assert-equal "and variance near 1/12"
              #t (< (abs (- (cdr um) (/ 1.0 12.0))) 0.005))

(define em (moments 20000 (lambda (r) (random-exponential r 2.0))))
(assert-equal "Exponential(2) has mean near 1/2"
              #t (< (abs (- (car em) 0.5)) 0.02))

;; Gamma's rejection loop is the one sampler that can fabricate a value.
;; It must essentially never do so at a reasonable alpha, and the count is
;; the only evidence either way.
(dist-reset-failures!)
(define gm (moments 5000 (lambda (r) (random-gamma r 2.0 1.0))))
(assert-equal "Gamma(2,1) has mean near 2"
              #t (< (abs (- (car gm) 2.0)) 0.1))
(assert-equal "and gives up on essentially none of 5000 draws"
              #t (< dist-failures 10))


;;--- splitting ----------------------------------------------------------
;; The same thing jax.random.split does, and for the same reason: split IS
;; Threefry, with the parent's output words becoming the child's key. So
;; coordinates and splitting are one primitive with two ergonomics, not
;; rival designs — which is why both can be offered without either being
;; a compromise.

(define parent (rng-make 0 42 0))
(define kid-a (rng-split! parent))
(define kid-b (rng-split! parent))

(assert-equal "two children of one parent draw differently"
              #f (= (rng-unit! (rng-split! (rng-make 0 42 0)))
                    (rng-unit! kid-b)))
(assert-equal "and neither repeats the parent"
              #f (= (rng-unit! kid-a) (rng-unit! (rng-make 0 42 0))))

;; Splitting must be a pure function of the parent's state, or nothing
;; downstream is reproducible from a seed.
(assert-equal "splitting is deterministic"
              (rng-unit! (rng-split! (rng-make 0 42 0)))
              (rng-unit! (rng-split! (rng-make 0 42 0))))

;; THE property split exists for. A child is insulated from its siblings'
;; DRAW COUNTS: change how many values one sub-computation consumes and
;; the others do not move. With a single generator threaded through, this
;; is false, and editing one submodel silently reshuffles every later one.
(define (sibling-b-after a-draws)
  (let* ((par (rng-make 0 42 0))
         (ca  (rng-split! par))
         (cb  (rng-split! par)))
    (let loop ((i 0)) (if (< i a-draws) (begin (rng-unit! ca) (loop (+ i 1)))))
    (rng-unit! cb)))
(assert-equal "a sibling's draw count does not move the other"
              (sibling-b-after 0) (sibling-b-after 25))

;; What split does NOT insulate against, stated so it is a known limit
;; rather than a surprise: the child's key depends on how many splits came
;; before it, so adding a sub-GF moves every later sibling. Hashing an
;; address would fix that, and is strictly stronger for exactly this
;; reason — see MANUAL section 5a.
(define (second-child-of splits-before)
  (let ((par (rng-make 0 42 0)))
    (let loop ((i 0)) (if (< i splits-before) (begin (rng-split! par) (loop (+ i 1)))))
    (rng-unit! (rng-split! par))))
(assert-equal "but adding an earlier sibling DOES move it"
              #f (= (second-child-of 0) (second-child-of 1)))

;; A split consumes exactly one block, so the parent advances by four.
(define sp (rng-make 0 7 0))
(rng-split! sp)
(assert-equal "a split costs the parent exactly four words"
              (let ((r (rng-make 0 7 0)))
                (rng-u32! r) (rng-u32! r) (rng-u32! r) (rng-u32! r)
                (rng-u32! r))
              (rng-u32! sp))


;;--- native erfc against the Scheme reference ---------------------------
;; Same arrangement as the Threefry core: the native version is the
;; implementation, the Scheme transcription beside lib/stat.wgsl is the
;; readable reference, and this is the chain between them. The constants
;; are asserted against the .wgsl text above, so agreeing here puts all
;; three in step.

(define (agrees-across? native reference lo hi steps tol)
  (let loop ((i 0))
    (cond ((> i steps) #t)
          (else (let ((x (+ lo (* (- hi lo) (/ i steps)))))
                  (if (> (abs (- (native x) (reference x))) tol) #f
                      (loop (+ i 1))))))))

(assert-equal "native erfc matches the Scheme reference across [-3, 3]"
              #t (agrees-across? erfc erfc/reference -3.0 3.0 200 1e-12))
(assert-equal "native inv-erf matches it across (-1, 1)"
              #t (agrees-across? inv-erf inv-erf/reference -0.999 0.999 200 1e-9))

;;--- bulk normals -------------------------------------------------------
;; 50M draws/second against 508K for the same loop in bytecode, which is
;; why this exists. A fast path that drew DIFFERENT numbers would be worse
;; than no fast path, so it is checked against the one-at-a-time sampler.

(define nb (make-bytes (* 32 4)))
(define nv (bytes-view nb :f32))
(rng-fill-normal! (rng-make 0 1234 0) nv 0 32 0.0 1.0)
(assert-equal "bulk normals match drawing one at a time"
              #t (let ((r (rng-make 0 1234 0)))
                   (let loop ((i 0))
                     (cond ((= i 32) #t)
                           ((> (abs (- (view-ref nv i) (random-normal r 0.0 1.0))) 1e-6) #f)
                           (else (loop (+ i 1)))))))

;; One uniform per normal, so a fill leaves the generator where the same
;; number of individual draws would. If the bulk path consumed a different
;; amount, mixing it with individual draws would silently desynchronise
;; the stream — and nothing about the values would look wrong.
(define fr (rng-make 0 55 0))
(rng-fill-normal! fr nv 0 32 0.0 1.0)
(assert-equal "a fill of 32 advances the generator by exactly 32 draws"
              (let ((r (rng-make 0 55 0)))
                (let loop ((i 0)) (if (< i 32) (begin (rng-unit! r) (loop (+ i 1)))))
                (rng-unit! r))
              (rng-unit! fr))

;; loc and scale must be applied, not ignored — an easy thing to drop in a
;; loop that is mostly about the transform.
(define sb (make-bytes (* 4000 4)))
(define sv (bytes-view sb :f32))
(rng-fill-normal! (rng-make 0 31415 0) sv 0 4000 10.0 2.0)
(assert-equal "a filled batch honours loc and scale"
              #t (let loop ((i 0) (s 0.0))
                   (if (= i 4000)
                       (< (abs (- (/ s 4000) 10.0)) 0.15)
                       (loop (+ i 1) (+ s (view-ref sv i))))))

(suite-summary)
