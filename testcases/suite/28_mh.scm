;;----------------------------------------------------------------------
;; Layer 28: random-walk Metropolis-Hastings over a staged log-joint
;;
;; lib/mh.scm is the rejuvenation half of the SMC pipeline, and the route
;; to it that asks nothing of the model but a log-joint. lib/gibbs.scm
;; refuses a model whose conditional has no closed form; this one does
;; not care, so every model that stages can be rejuvenated.
;;
;; HOW AN MH KERNEL IS TESTED AT ALL. A chain cannot be compared against
;; an expected value sample by sample, and its correctness is a statement
;; about what it converges to rather than about any one step. So the
;; subject here is a model with ONE latent, whose conditional is therefore
;; its posterior, and whose posterior lib/gibbs.scm gives in closed form.
;; What the chain converges to is then checkable against a number rather
;; than against another chain.
;;
;; The structural assertions carry the rest, and one of them is not
;; cosmetic: the proposal must be drawn ONCE per sweep and shared by every
;; attribute the sweep writes. The terminal form compiles each of its
;; attribute arguments separately, so a proposal written inside them would
;; be redrawn per attribute and the particle would move to a different
;; point in each coordinate — while looking entirely plausible.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")
;; mh.scm before gpu.scm: it loads stage.scm, which loads wgsl.scm and
;; resets the terminal table that wrangle.scm registers `point` into.
(load "lib/mh.scm")
(load "lib/gibbs.scm")
(load "lib/gpu.scm")

(test-suite "28_mh: random-walk MH over a staged log-joint")

(define (refused? thunk) (guard (e (#t #t)) (thunk) #f))
(define (close? a b tol) (< (abs (- a b)) tol))

;;--- a model whose posterior is known -----------------------------------

(define NPTS 12)
(define ys (bytes-view (make-bytes (* NPTS 8)) :f64))
(let loop ((j 0))
  (if (< j NPTS)
      (begin (view-set! ys j (+ 1.7 (* 0.3 (- j 6)))) (loop (+ j 1)))))

(define-gen (one npts)
  (let ((m (at :m (normal 0 2.0))))
    (at :ys (batch-i npts (j) (normal m 1.0)))))

(define st (stage (one NPTS)))
(define truth (gibbs-posterior st (gibbs-structure st :m) {:m 0.0 :ys ys}))

;;--- what the chain converges to ---------------------------------------
;; The load-bearing test. Twelve observations against a Normal(0, 2)
;; prior, so the posterior is tight and a chain that is merely plausible
;; will miss it.

(define chain
  (let ((r (rng-make 0 1234 0)))
    (let loop ((i 0) (ch {:m 0.0 :ys ys}) (n 0) (s 0.0) (s2 0.0) (taken 0))
      (if (= i 20000)
          (let* ((mean (/ s n)))
            (list mean
                  (sqrt (- (/ s2 n) (* mean mean)))
                  (/ (exact->inexact taken) (exact->inexact n))))
          (let* ((res (mh-sweep st ch 0.8 r))
                 (ch2 (car res)))
            ;; A burn-in, because the chain starts at 0.0 and the
            ;; posterior is near 1.5 — the first few hundred samples are
            ;; the walk over, not the distribution.
            (if (< i 2000)
                (loop (+ i 1) ch2 n s s2 taken)
                (loop (+ i 1) ch2 (+ n 1)
                      (+ s (map-ref ch2 :m))
                      (+ s2 (* (map-ref ch2 :m) (map-ref ch2 :m)))
                      (if (cdr res) (+ taken 1) taken))))))))

;; Tolerances in units of the posterior's own scale, which is the only
;; scale-free way to say it: a chain wrong by a tenth of a standard
;; deviation is wrong, and one wrong by 1e-5 of one is arithmetic.
(assert-true "the chain's mean is the posterior's mean"
             (close? (car chain) (car truth) (* 0.15 (cdr truth))))
(assert-true "and its spread is the posterior's spread"
             (close? (cadr chain) (cdr truth) (* 0.05 (cdr truth))))

;; The diagnostic that says whether a step size is usable at all, and the
;; one thing about MH that no sample inspection reveals.
(assert-true "the acceptance rate is in the usable band"
             (and (> (caddr chain) 0.15) (< (caddr chain) 0.7)))

;; Both ends of the step-size failure, because they look identical in the
;; samples and opposite here: a huge step never lands, a tiny one always
;; does and goes nowhere.
(assert-true "a step far too large is almost never accepted"
             (< (cdr (mh-run st {:m 1.5 :ys ys} 500.0 (rng-make 0 7 0) 300)) 0.05))
(assert-true "and one far too small is almost always accepted"
             (> (cdr (mh-run st {:m 1.5 :ys ys} 1e-4 (rng-make 0 7 0) 300)) 0.9))

;;--- the log-joint as a callable kernel function -----------------------
;; An accept ratio needs the log-joint at two parameter values. Inlining
;; the staged expression twice would emit its likelihood fold twice; a
;; function emits it once and is called twice. That the staged kernel's
;; free names ARE the scalar choices is what makes the registration free.

(wgsl-declare! 'ys "ys_at" '(:u32) :f32)

(assert-equal "the parameters are the scalar choices, in source order"
              '(m) (staged-logjoint-fn! st 'lj1))
(assert-equal "and the function scores to a scalar"
              :f32 (wgsl-type '(lj1 0.5) '()))

(assert-true "a model with no scalar choice has nothing to rejuvenate"
             (let ()
               (define-gen (allobs npts)
                 (at :ys (batch-i npts (j) (normal 0.0 1.0))))
               (refused? (lambda ()
                           (staged-logjoint-fn! (stage (allobs 3)) 'lj2)))))

;;--- the emitted sweep --------------------------------------------------

(define-dual (mh-curve-elem (x :f32) (a :f32) (b :f32) (c :f32))
  (+ (* a x x) (* b x) c))

(define (f32v n) (let ((b (make-bytes (* n 4)))) (bytes-seal! b) (bytes-view b :f32)))
(define xs3 (f32v 8))

(define-gen (curve3 xs sigma npts)
  (let* ((a (at :a (normal 0 1.5)))
         (b (at :b (normal 0 1.5)))
         (c (at :c (normal 0 1.5))))
    (at :ys (batch-i npts (j)
              (normal (mh-curve-elem (view-ref xs j) a b c) sigma)))))

(define st3 (stage (curve3 xs3 1.0 8)))

(scratch-attributes! '((a :f32) (b :f32) (c :f32) (taken :f32)))
(shared-layout! '((xs 8) (ys 8)))
(define-gpu (xs (j :u32)) (shared-xs j))
(define-gpu (ys (j :u32)) (shared-ys j))
(wrangle-params! '(mhstep))

(assert-equal "three scalar choices become three parameters"
              '(a b c) (staged-logjoint-fn! st3 'logjoint))

(define msrc (wrangle-scheme (staged-mh-body st3 'logjoint 'mhstep 'taken)))

(define (main-of src)
  ;; The emitted module holds the whole library plus `fn logjoint` plus
  ;; `fn main`, so counting a name across all of it counts definitions and
  ;; declarations too. The sweep is what these assertions are about.
  (let ((n (string-length src)))
    (let loop ((i 0))
      (cond ((> (+ i 7) n) src)
            ((string=? (substring src i (+ i 7)) "fn main") (substring src i n))
            (else (loop (+ i 1)))))))

(define (fn-of src name)
  ;; The body of one emitted function, so a count can be about it rather
  ;; than about the module — which holds the whole library and, in this
  ;; suite, two registered log-joints.
  (let* ((open (string-append "fn " name "("))
         (n    (string-length src))
         (k    (string-length open)))
    (let find ((i 0))
      (cond ((> (+ i k) n) "")
            ((string=? (substring src i (+ i k)) open)
             (let close ((j i))
               (cond ((> (+ j 2) n) (substring src i n))
                     ((string=? (substring src j (+ j 2)) "\n}") (substring src i j))
                     (else (close (+ j 1))))))
            (else (find (+ i 1)))))))

(define (occurrences hay needle)
  (let ((n (string-length needle)))
    (let loop ((i 0) (k 0))
      (if (> (+ i n) (string-length hay))
          k
          (loop (+ i 1)
                (if (string=? (substring hay i (+ i n)) needle) (+ k 1) k))))))

;; Twice, not more: once at the current parameters and once at the
;; proposed. A third call would mean something was recomputed.
(define mmain (main-of msrc))

(assert-equal "the sweep calls the log-joint exactly twice"
              2 (occurrences mmain "logjoint("))

;; And the likelihood fold is emitted ONCE, inside the function — so the
;; sweep has none of its own, which is the whole reason the log-joint
;; became a function rather than being inlined at both call sites.
(assert-equal "its likelihood fold is declared once, inside the function"
              1 (occurrences (fn-of msrc "logjoint") "var acc_ys"))
(assert-equal "and not at all in the sweep"
              0 (occurrences mmain "var acc_ys"))

;; The consumption order is contract: the host twin draws one normal per
;; address in source order and then one uniform, and the two can only be
;; compared if the kernel does the same.
(assert-true "the proposal draws one normal per address, then one uniform"
             (let ((na (occurrences mmain "random_normal("))
                   (nu (occurrences mmain "random_uniform(")))
               (and (= na 3) (= nu 1))))

;; THE ONE THAT MATTERS. Every attribute must select between its own
;; current value and the SAME proposal — so each proposal name appears
;; exactly once in a select, and the decision bool is shared.
(assert-true "each coordinate writes the proposal that was actually drawn"
             (and (string-contains? msrc "attr_a_set(i, select(attr_a(i), mh_p_a_5,")
                  (string-contains? msrc "attr_b_set(i, select(attr_b(i), mh_p_b_6,")
                  (string-contains? msrc "attr_c_set(i, select(attr_c(i), mh_p_c_7,")))
;; One binding and four uses: the decision is made once and every write
;; reads it, rather than each coordinate deciding for itself.
(assert-equal "under one shared decision, bound once and read four times"
              5 (occurrences mmain "mh_take_10"))

;; The step is a live parameter rather than a baked constant, so a
;; workbench can tune it against the acceptance rate without recompiling.
(assert-true "the step size reaches the kernel as a live parameter"
             (string-contains? msrc "random_normal(0.0, w.p0)"))

(suite-summary)
