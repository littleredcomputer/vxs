;;; Generative functions over fibers.
;;;
;;; A model is an ordinary procedure that calls `at` where it makes a
;;; random choice. `at` yields; a driver on the other side of that yield
;;; decides what the choice is worth and hands a value back. Nothing about
;;; the model is transformed, declared, or annotated.
;;;
;;; WHY THAT IS THE WHOLE TRICK. Systems that do this in a language without
;;; suspension have to synthesise it: WebPPL CPS-transforms a subset of
;;; JavaScript; GenJAX decorates and traces to a jaxpr; a tree-sitter
;;; rewrite would insert the plumbing into the source text. All of them are
;;; buying inversion of control, and a coroutine already has it.
;;;
;;; It also hides the generator. A model never mentions an RNG key, because
;;; the model is not running when a draw happens — it is suspended, and the
;;; driver holds the key, the trace and the accumulators. That is the
;;; second transformation (threading keys through every call site) made
;;; unnecessary by the same mechanism.
;;;
;;; What it costs, and it is a real cost: a live coroutine cannot be
;;; INSPECTED. A jaxpr can be compiled, differentiated, or vectorised; this
;;; can only be run. See MANUAL section 2 on where a fiber may suspend, and
;;; the note on `batch` below for where the two meet.

(load "lib/dist.scm")

;;--- distributions ------------------------------------------------------
;; A vtable held as a RECORD rather than a `case` over symbols. The
;; difference is where a typo lands: (d:sampel d k) is an unbound variable
;; at compile time, while a mistyped message falls to an `else` clause at
;; run time — or, with no else, silently does nothing.
;;
;; Four capabilities, two interfaces. `sample`/`score` are what a driver
;; speaks; `fill`/`sum` exist so `batch` can convert them into the first
;; pair at a larger shape. A driver never learns fill and sum exist.

(define-record-type <distribution>
  (make-dist form sample score fill sum) distribution?
  (form   dist-form)
  (sample dist-sample)
  (score  dist-score)
  (fill   dist-fill)
  (sum    dist-sum))

(define (d:sample d k)                ((dist-sample d) k))
(define (d:score  d v)                ((dist-score  d) v))
(define (d:fill!  d k view start n)   ((dist-fill   d) k view start n))
(define (d:sum    d view start n)     ((dist-sum    d) view start n))

;; What goes in a slot a distribution does not have. Better than #f:
;; calling it names what is missing and from what, where #f would only say
;; "attempted to call non-procedure".
(define (unsupported what form)
  (lambda args (error 'distribution "does not support" what form)))

;; (distribution name sample score fill sum) -> a family.
;; Applying the family to its parameters gives a distribution, with the
;; parameters already closed over — no caller passes them again.
(define (distribution name sample score fill sum)
  (lambda args
    (let ((form `(,name ,@args)))
      (make-dist form
                 (lambda (k)              (apply sample k args))
                 (lambda (v)              (apply score v args))
                 (lambda (k view start n) (apply fill k view start n args))
                 (lambda (view start n)   (apply sum view start n args))))))

;; The standard families, over lib/dist.scm.
(define normal  (distribution 'normal  random-normal  logpdf-normal  fill-normal!  logpdf-sum-normal))
(define uniform (distribution 'uniform random-uniform logpdf-uniform fill-uniform! logpdf-sum-uniform))
(define flip    (distribution 'flip    random-flip    logpdf-flip    fill-flip!    logpdf-sum-flip))

;; (batch d n) — one choice whose value is n draws.
;;
;; Converts d's (fill, sum) into (sample, score) at a larger shape, and is
;; itself a distribution, which is what lets it sit at an address. That is
;; the vectorisation seam.
;;
;; It has no fill or sum of its own, and the record makes that visible in
;; the constructor rather than discoverable by falling through an `else`.
;; So (batch (batch d n) m) cannot work — a two-axis shape wants a
;; different mechanism, not a nested one.
(define (batch d n)
  (let ((form `(batch ,(dist-form d) ,n)))
    (make-dist form
               (lambda (k)
                 (let ((view (bytes-view (make-bytes (* n 4)) :f32)))
                   (d:fill! d k view 0 n)
                   view))
               (lambda (v) (d:sum d v 0 n))
               (unsupported 'fill form)
               (unsupported 'sum  form))))

;;--- generative functions -----------------------------------------------

(define-record-type <generative-function>
  (make-gf f args) generative-function?
  (f gf-f)
  (args gf-args))

(define (gf f) (lambda args (make-gf f args)))

;; (define-gen (model a b) body ...) — `model` becomes a CONSTRUCTOR, so
;; (model 1 2) builds a generative function rather than running one.
;;
;; This is not GenJAX's @gen. That decorator performs a tracing
;; transformation on the body; this touches the body not at all — the
;; coroutine already does that work. All it buys is one name instead of
;; two, and no way to write the wrong one.
(defmacro (define-gen spec . body)
  `(define ,(car spec) (gf (lambda ,(cdr spec) ,@body))))

;; A random choice. Yields (address thing) and receives back whatever the
;; driver decides the choice is.
;;
;; The guard is not decorative. (yield) outside a generator is legal — the
;; scheduler resumes it with unspecified — so a model called directly would
;; run, receive unspecified for every choice, and return a plausible number,
;; because arithmetic here does not type-check. See in-generator? in
;; MANUAL section 1.
(define (at address thing)
  (if (not (in-generator?))
      (error 'at "not inside a generative function" address))
  (yield (list address thing)))

;;--- the driver ---------------------------------------------------------
;; Knows nothing about distributions: resume, hand what came out to the
;; stepper, hand back what the stepper returns. Everything that differs
;; between simulating and assessing lives in the stepper.

(define-record-type <stepper>
  (make-stepper step finish) stepper?
  (step   stepper-step-fn)
  (finish stepper-finish-fn))

(define (stepper-step   s d) ((stepper-step-fn   s) d))
(define (stepper-finish s d) ((stepper-finish-fn s) d))

(define (gf-driver gf stepper)
  (assert (generative-function? gf))
  (let ((g (apply generator (gf-f gf) (gf-args gf))))
    (let loop ((d (resume g)))
      (if (generator-live? g)
          (loop (resume g (stepper-step stepper d)))
          (stepper-finish stepper d)))))

;;--- simulating ---------------------------------------------------------
;; Draws every choice and records what it drew.
;;
;; A nested generative function gets its own generator via rng-split!, so
;; a sub-model is insulated from its siblings' DRAW COUNTS: change how many
;; values one consumes and the others do not move. The parent is resumed
;; with the sub-model's RETURN VALUE, while the sub-TRACE goes at the
;; address — those are different things, and conflating them is silent.

(define (sample-stepper k)
  (let ((tr {})
        (sum-w 0.0))
    (make-stepper
     (lambda (d)
       (cond
        ((distribution? (cadr d))
         (let* ((address (car d))
                (distro  (cadr d))
                (retval  (d:sample distro k))
                (w       (d:score distro retval)))
           (map-set! tr address {:score w :retval retval})
           (set! sum-w (+ sum-w w))
           retval))
        ((generative-function? (cadr d))
         (let* ((address  (car d))
                (sub      (cadr d))
                (sub-tr   (gf-driver sub (sample-stepper (rng-split! k)))))
           (map-set! tr address sub-tr)
           (set! sum-w (+ sum-w (:score sub-tr)))
           (:retval sub-tr)))
        (else (raise `(trace? ,d)))))
     (lambda (d)
       (map-set! tr :retval d)
       (map-set! tr :score sum-w)
       tr))))

(define (sample gf seed) (gf-driver gf (sample-stepper (rng-make 0 seed 0))))

;;--- assessing ----------------------------------------------------------
;; Draws nothing. Every address must be constrained; a missing one is a
;; caller error rather than a zero-probability event, and saying so at the
;; level where it is missing matters — without the check below, a missing
;; sub-map reports the first leaf INSIDE it, naming a grandchild when the
;; child is what was forgotten.

(define (assess-stepper choices)
  (let ((sum-w 0.0))
    (make-stepper
     (lambda (d)
       (cond
        ((distribution? (cadr d))
         (let ((address (car d))
               (distro  (cadr d)))
           (if (not (map-has? choices address))
               (raise `(missing-choice ,address)))
           (let* ((choice (address choices))
                  (w      (d:score distro choice)))
             (set! sum-w (+ sum-w w))
             choice)))
        ((generative-function? (cadr d))
         (let ((address (car d))
               (sub     (cadr d)))
           (if (not (map-has? choices address))
               (raise `(missing-choice ,address)))
           (let* ((result  (gf-driver sub (assess-stepper (map-ref choices address))))
                  (inner-w (car result))
                  (retval  (cdr result)))
             (set! sum-w (+ sum-w inner-w))
             retval)))
        (else (raise `(assess? ,d)))))
     (lambda (d) (cons sum-w d)))))

(define (assess gf choices) (gf-driver gf (assess-stepper choices)))


;;--- importance ---------------------------------------------------------
;; K samples of a model, with some addresses constrained.
;;
;; TRANSPOSED, and that is the whole of the design. N traces would be N
;; maps plus one map per address inside each — at K=20000 that measured
;; 10.9MB live, against 0.41MB for the columns below, and the ratio grows
;; with the number of addresses. Nothing about the call site fixes that;
;; it is structural, so the structure changes.
;;
;;   {address -> :f64 view of K, ..., :weights view of K, :n K}
;;
;; NOT vectorised. This is a loop over K, one generator per sample; only
;; the OUTPUT is columnar. Vectorising properly needs distribution
;; parameters that may be views, so that one pass covers all K — at which
;; point the per-sample generator goes away too, and that is currently
;; where the time goes (20000 samples cost 1819 collections, and 20000
;; bare generators cost 2500).

(define (column! cols address K)
  (if (map-has? cols address)
      (map-ref cols address)
      (let ((v (bytes-view (make-bytes (* K 8)) :f64)))
        (map-set! cols address v)
        v)))

;; TWO accumulators, and the asymmetry is the point: everything goes into
;; SCORE, only constrained values go into WEIGHT.
;;
;; The unconstrained choices are drawn from the prior, so their density
;; appears in both numerator and denominator and cancels exactly. Adding
;; them to the weight — the obvious thing, since both branches compute a
;; logpdf — gives a number that is plausible, moves in roughly the right
;; direction, and is wrong.
(define (importance-stepper constraints cols i K k)
  (let ((weight 0.0) (score 0.0))
    (make-stepper
     (lambda (d)
       (let ((address (car d)) (thing (cadr d)))
         (cond
          ;; Refused rather than fudged: columns are keyed by a single
          ;; address, so a nested model needs path-flattened keys — a
          ;; decision, not an oversight.
          ((not (distribution? thing))
           (raise `(importance: nested-generative-function-unsupported ,address)))
          ((map-has? constraints address)
           (let* ((v (map-ref constraints address))
                  (w (d:score thing v)))
             (set! weight (+ weight w))
             (set! score  (+ score w))
             v))
          (else
           (let* ((v (d:sample thing k))
                  (w (d:score thing v)))
             ;; An unconstrained BATCHED choice would want a K-by-n
             ;; column. That is the two-axis shape we do not build, so it
             ;; is refused where it happens rather than silently coerced.
             (if (not (number? v))
                 (raise `(importance: unconstrained-batch-needs-a-2d-column ,address)))
             (set! score (+ score w))
             (view-set! (column! cols address K) i v)
             v)))))
     (lambda (d) (list weight score d)))))

;; Sample i draws from (rng-make i seed 0) — ptnum is the sample index, so
;; sample 37 is the same whether you drew 40 or 40,000, in any order. That
;; is the axis rng-make's coordinates exist for, and it means no splitting
;; and no threading between samples.
(define (importance gf constraints K seed)
  (let ((cols {})
        (weights (bytes-view (make-bytes (* K 8)) :f64)))
    (let loop ((i 0))
      (if (< i K)
          (let ((r (gf-driver gf (importance-stepper constraints cols i K
                                                     (rng-make i seed 0)))))
            (view-set! weights i (car r))
            (loop (+ i 1)))))
    (map-set! cols :weights weights)
    (map-set! cols :n K)
    cols))

;; The normalised weights, in place. Subtracting logsumexp first is what
;; keeps this finite: raw log-weights routinely sit below -700, where exp
;; underflows to zero and every particle looks equally impossible.
(define (normalize-weights! ws K)
  (let ((lse (logsumexp ws 0 K)))
    (let loop ((i 0))
      (if (< i K)
          (begin (view-set! ws i (exp (- (view-ref ws i) lse)))
                 (loop (+ i 1)))))))

;; The weighted mean of one column, without materialising anything.
(define (weighted-mean col ws K)
  (let ((lse (logsumexp ws 0 K)))
    (let loop ((i 0) (m 0.0))
      (if (= i K) m
          (loop (+ i 1) (+ m (* (exp (- (view-ref ws i) lse)) (view-ref col i))))))))
