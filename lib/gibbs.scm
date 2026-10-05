;;; Conjugate structure, READ from the staged IR.
;;;
;;; A Gibbs update for one address needs its conditional given everything
;;; else, and for a conjugate pair that conditional has a closed form. This
;;; file answers the prior question: does the model, as written, HAVE that
;;; form at this address — and if so, what are its coefficients.
;;;
;;; It reads lib/stage.scm's IR rather than the model's source, because the
;;; IR is already normal form: names resolved, parameters folded to
;;; literals, `(choice a)` distinguished from `(data xs j)`, and the terms
;;; flattened to summands in source order. The structural question is a
;;; scan over that datum.
;;;
;;; STATIC, NOT PROBED, and the distinction is load-bearing. A conjugate
;;; update is derived from a CLAIM about structure — this prior has these
;;; children, entering their means this way. If the claim is wrong the
;;; chain converges to the wrong stationary distribution, and no oracle
;;; catches it, because a CPU replay stepper derives the update from the
;;; same claim and makes the identical mistake. Both interpreters are then
;;; wrong in the same direction, which is the one failure a two-path design
;;; cannot see. So nothing here is witnessed by running the model: every
;;; refusal below is a proof from the text.
;;;
;;; WHY THE HELPER BODIES ARE NEEDED. The IR keeps a call opaque, so the
;;; curve model's mean reads
;;;
;;;   (call curve-elem (data xs j) (choice :a) (choice :b) (choice :c))
;;;
;;; and whether that is affine in :a — the whole question for a
;;; Normal-Normal update — is invisible from outside. lib/wgsl.scm keeps
;;; each registered body for exactly this, and the analysis below inlines
;;; it. The set of readable bodies is the set of functions that paid the
;;; entry tax, so a helper the kernel could never have called is also one
;;; this cannot see into, and says so by name.

(load "lib/stage.scm")

;;--- does an expression mention this address? ---------------------------
;; The cheap question, asked first everywhere below: an expression that
;; does not mention the address is a CONSTANT for this update, whatever
;; else it contains. That is what lets a model call `erfc` or read a
;; buffer through an unreadable accessor without the analysis caring — it
;; only has to see into the functions the address actually flows through.

(define (ir-mentions? e addr)
  (cond
    ((not (pair? e)) #f)
    ((and (eq? (car e) 'choice) (eq? (cadr e) addr)) #t)
    (else (let loop ((xs e))
            (cond ((not (pair? xs)) #f)
                  ((ir-mentions? (car xs) addr) #t)
                  (else (loop (cdr xs))))))))

;;--- building IR, with the identities folded ----------------------------
;; The coefficients come out as IR expressions rather than numbers, since
;; they generally depend on the loop index and on the other choices. These
;; fold 0 and 1 on the way, which is not cosmetic: it is what makes a
;; scalar mean come out as coefficient 1 and offset 0 exactly, so a caller
;; can recognise the simple case instead of emitting (* 1.0 x).

(define (ir-zero? e) (and (number? e) (= e 0)))
(define (ir-one? e)  (and (number? e) (= e 1)))

(define (ir-add a b)
  (cond ((ir-zero? a) b) ((ir-zero? b) a)
        ((and (number? a) (number? b)) (+ a b))
        (else (list '+ a b))))

(define (ir-sub a b)
  (cond ((ir-zero? b) a)
        ((and (number? a) (number? b)) (- a b))
        (else (list '- a b))))

(define (ir-mul a b)
  (cond ((or (ir-zero? a) (ir-zero? b)) 0)
        ((ir-one? a) b) ((ir-one? b) a)
        ((and (number? a) (number? b)) (* a b))
        (else (list '* a b))))

(define (ir-div a b)
  (cond ((ir-one? b) a)
        ((ir-zero? a) 0)
        (else (list '/ a b))))

;;--- inlining a registered helper ---------------------------------------
;; A body is written in the kernel language, where a call to another
;; function is a bare application — `(curve-elem x a b c)` — while the
;; staged IR wraps one as `(call curve-elem ...)`. Substitution therefore
;; rewrites applications into the IR's form, so the result is ordinary IR
;; and the affine reader below handles nesting without a second path.

(define (ir-inline e)
  (let* ((name (cadr e))
         (args (cddr e))
         (b    (wgsl-fn-body name)))
    (if (not b)
        (error 'gibbs
               (string-append (symbol->string name)
                              ": declared for the device but has no body in this"
                              " language, so whether the address enters it"
                              " affinely cannot be read. Define it with"
                              " define-dual or define-gpu to make it readable")
               name))
    (let ((params (map car (wgsl-body-params b))))
      (if (not (= (length params) (length args)))
          (error 'gibbs "a helper was called with the wrong arity" name))
      (ir-subst (wgsl-body-expr b) (map cons params args)))))

(define (ir-subst e sub)
  (cond
    ((number? e) e)
    ((symbol? e)
     (let ((b (assq e sub)))
       (if b
           (cdr b)
           ;; The kernel language has no globals, so a free name in a body
           ;; is not something to resolve later — it is a name that cannot
           ;; mean anything here.
           (error 'gibbs "a helper's body names something that is not a parameter" e))))
    ((pair? e)
     (let ((head (car e)))
       (cond
         ((memq head '(+ - * /))
          (cons head (map (lambda (x) (ir-subst x sub)) (cdr e))))
         ;; Anything else applied to arguments is a call to another
         ;; registered function; rewrite it into the IR's shape.
         ((symbol? head)
          (cons 'call (cons head (map (lambda (x) (ir-subst x sub)) (cdr e)))))
         (else (error 'gibbs "a helper's body is not in the arithmetic subset" e)))))
    (else (error 'gibbs "a helper's body is not in the arithmetic subset" e))))

;;--- the affine reader -------------------------------------------------
;; (ir-affine e addr) -> (coeff . offset), where e is exactly
;;
;;     offset + coeff * (choice addr)
;;
;; or an error naming why it is not. Refusing is the point: a mean that is
;; quadratic in the address has no Normal-Normal update, and silently
;; linearising it would produce a chain that mixes and converges to the
;; wrong thing.

(define (ir-affine e addr)
  (cond
    ;; Not present at all: the whole expression is the offset. This is the
    ;; base case that keeps the analysis shallow — it never descends into
    ;; a subtree the address does not reach.
    ((not (ir-mentions? e addr)) (cons 0 e))
    ((and (pair? e) (eq? (car e) 'choice)) (cons 1 0))
    ((pair? e)
     (let ((head (car e)))
       (cond
         ((eq? head '+) (ir-affine-sum (cdr e) addr))
         ((eq? head '-) (ir-affine-diff (cdr e) addr))
         ((eq? head '*) (ir-affine-prod (cdr e) addr))
         ((eq? head '/) (ir-affine-quot (cdr e) addr))
         ((eq? head 'call) (ir-affine (ir-inline e) addr))
         ;; A batched choice or a buffer read indexed BY the address would
         ;; be a gather, which staging refuses upstream for its own
         ;; reasons; say so here too rather than fall through.
         (else (error 'gibbs
                      "the address enters an expression that is not arithmetic"
                      head)))))
    (else (error 'gibbs "the address enters an expression that cannot be read" e))))

(define (ir-affine-sum args addr)
  (let loop ((as args) (c 0) (o 0))
    (if (null? as)
        (cons c o)
        (let ((p (ir-affine (car as) addr)))
          (loop (cdr as) (ir-add c (car p)) (ir-add o (cdr p)))))))

(define (ir-affine-diff args addr)
  (let ((first (ir-affine (car args) addr)))
    (if (null? (cdr args))
        (cons (ir-sub 0 (car first)) (ir-sub 0 (cdr first)))   ; unary negate
        (let loop ((as (cdr args)) (c (car first)) (o (cdr first)))
          (if (null? as)
              (cons c o)
              (let ((p (ir-affine (car as) addr)))
                (loop (cdr as) (ir-sub c (car p)) (ir-sub o (cdr p)))))))))

;; At most ONE factor may carry the address, or the product is at least
;; quadratic in it. (alpha1 + beta1 a) * alpha2 = alpha1 alpha2 + beta1 alpha2 a.
(define (ir-affine-prod args addr)
  (let loop ((as args) (ps '()))
    (if (not (null? as))
        (loop (cdr as) (cons (ir-affine (car as) addr) ps))
        (let ((ps (reverse ps)))
          (let count ((xs ps) (n 0))
            (cond
              ((not (null? xs))
               (count (cdr xs) (if (ir-zero? (car (car xs))) n (+ n 1))))
              ((> n 1)
               (error 'gibbs
                      "the address appears in more than one factor of a product,"
                      'nonlinear))
              (else
               ;; The rest of the product, which by the count above is
               ;; free of the address.
               (let rest ((xs ps) (acc 1) (carrier #f))
                 (cond
                   ((null? xs)
                    (if carrier
                        (cons (ir-mul (car carrier) acc) (ir-mul (cdr carrier) acc))
                        (cons 0 acc)))
                   ((and (not carrier) (not (ir-zero? (car (car xs)))))
                    (rest (cdr xs) acc (car xs)))
                   (else
                    (rest (cdr xs) (ir-mul acc (cdr (car xs))) carrier)))))))))))

;; The address may not appear in a divisor: 1/a is not affine in a.
(define (ir-affine-quot args addr)
  (let ((num (ir-affine (car args) addr)))
    (let loop ((as (cdr args)) (den 1))
      (cond
        ((null? as) (cons (ir-div (car num) den) (ir-div (cdr num) den)))
        ((ir-mentions? (car as) addr)
         (error 'gibbs "the address appears in a divisor, which is not affine" 'divisor))
        (else (loop (cdr as) (ir-mul den (car as))))))))

;;--- reading a term ----------------------------------------------------

(define (term-score t)
  (cond
    ((eq? (car t) 'score) (list #f #f t))
    ((eq? (car t) 'sum-over)
     (let ((inner (cadddr t)))
       (if (not (eq? (car inner) 'score))
           (error 'gibbs "a child term nests a reduction inside a reduction" t))
       (list (cadr t) (caddr t) inner)))
    (else (error 'gibbs "a scan is not a conjugate child" (car t)))))

(define (prior-term? t addr)
  (and (eq? (car t) 'score) (equal? (caddr t) (list 'choice addr))))

;;--- the entry point ---------------------------------------------------
;; (gibbs-structure st addr) -> a description of the conditional, or an
;; error naming what stopped it.
;;
;;   {:addr     the address
;;    :kind     'normal-normal
;;    :prior    (loc scale)                     ; IR exprs
;;    :children ((n idx coeff offset scale value) ...)}
;;
;; `n` is #f for a scalar child and the bound for a batched one, `idx` its
;; index name. Everything else is an IR expression, so a backend can
;; evaluate these on the VM or emit them for a device without this file
;; knowing which.
;;
;; The prior's own parameters MAY mention other choices. That is not
;; hierarchy leaking in — a Gibbs conditional holds every other address
;; fixed, so another choice appearing here is a conditioned constant. Only
;; the address itself is forbidden, since a prior that mentions what it
;; scores is not a prior.

(define (gibbs-structure st addr)
  (let ((shape (assq addr (:choices st))))
    (if (not shape)
        (error 'gibbs "no such address in this model" addr))
    (if (not (eq? (cadr shape) 'scalar))
        (error 'gibbs "a batched choice has no scalar conjugate update" addr))
    (let loop ((ts (:terms st)) (prior #f) (kids '()))
      (cond
        ((not (null? ts))
         (let ((t (car ts)))
           (cond
             ((prior-term? t addr)
              (if prior (error 'gibbs "two terms score the same address" addr))
              (loop (cdr ts) t kids))
             ((ir-mentions? t addr)
              (loop (cdr ts) prior (cons (gibbs-child t addr) kids)))
             (else (loop (cdr ts) prior kids)))))
        ((not prior)
         (error 'gibbs "no term scores this address, so it has no prior" addr))
        (else
         (let ((pd (cadr prior)))
           (if (not (eq? (car pd) 'normal))
               (error 'gibbs "only a Normal prior has a Normal-Normal update"
                      (car pd)))
           (if (ir-mentions? pd addr)
               (error 'gibbs "a prior that mentions what it scores is not a prior"
                      addr))
           {:addr     addr
            :kind     'normal-normal
            :prior    (list (cadr pd) (caddr pd))
            :children (reverse kids)}))))))

(define (gibbs-child t addr)
  (let* ((parts (term-score t))
         (n     (car parts))
         (idx   (cadr parts))
         (score (caddr parts))
         (dist  (cadr score))
         (value (caddr score)))
    (if (not (eq? (car dist) 'normal))
        (error 'gibbs "a child of a Normal prior must be Normal to be conjugate"
               (car dist)))
    (if (ir-mentions? value addr)
        (error 'gibbs "the address appears in what a child SCORES, not in its mean"
               addr))
    (if (ir-mentions? (caddr dist) addr)
        (error 'gibbs "the address appears in a child's scale, which is not conjugate"
               addr))
    (let ((p (ir-affine (cadr dist) addr)))
      (if (ir-zero? (car p))
          (error 'gibbs "a child mentions the address but its mean does not depend on it"
                 addr))
      (list n idx (car p) (cdr p) (caddr dist) value))))

;;--- the update, on the fiber path -------------------------------------
;; The conditional for a Normal prior with Normal children is Normal, and
;; its natural parameters are a SUM over the children — which is the
;; reason a conjugate update is a filter rather than a re-derivation:
;;
;;   precision = 1/s0^2 + sum beta^2 / sigma^2
;;   mean      = (m0/s0^2 + sum beta (y - alpha) / sigma^2) / precision
;;
;; Every quantity comes out of the structure as an IR expression, so this
;; evaluates them with stage.scm's own backend. The point of reusing
;; staged-value rather than writing a second evaluator is that the
;; coefficients are then read by exactly the code that reads the rest of
;; the IR, in the same precision, with the same rules for what an index
;; and a buffer mean.

(define (gibbs-child-fold k st choices prec num)
  (let ((n      (car k))
        (idx    (cadr k))
        (coeff  (caddr k))
        (offset (cadddr k))
        (scale  (list-ref k 4))
        (value  (list-ref k 5)))
    (define (one env prec num)
      (let* ((b  (staged-value coeff  st choices env))
             (al (staged-value offset st choices env))
             (sg (staged-value scale  st choices env))
             (y  (staged-value value  st choices env))
             (w  (/ 1.0 (* sg sg))))
        (cons (+ prec (* b b w))
              (+ num  (* b (- y al) w)))))
    (if (not n)
        (one '() prec num)
        (let loop ((j 0) (prec prec) (num num))
          (if (= j n)
              (cons prec num)
              (let ((r (one (list (cons idx j)) prec num)))
                (loop (+ j 1) (car r) (cdr r))))))))

;; (gibbs-posterior st g choices) -> (loc . scale)
(define (gibbs-posterior st g choices)
  (let* ((pr (:prior g))
         (m0 (staged-value (car pr)  st choices '()))
         (s0 (staged-value (cadr pr) st choices '()))
         (p0 (/ 1.0 (* s0 s0))))
    (let loop ((ks (:children g)) (prec p0) (num (* m0 p0)))
      (if (null? ks)
          (cons (/ num prec) (sqrt (/ 1.0 prec)))
          (let ((r (gibbs-child-fold (car ks) st choices prec num)))
            (loop (cdr ks) (car r) (cdr r)))))))

;;--- and an oracle for it ----------------------------------------------
;; The header above says a Gibbs update cannot be checked by comparing two
;; backends, because a second derivation from the same structural claim
;; repeats the same mistake. That is true of a SECOND DERIVATION. It is not
;; true of a measurement of the model itself.
;;
;; When the conditional is Gaussian the log-joint is EXACTLY quadratic in
;; the address, so three evaluations of it determine that quadratic
;; completely — no fit, no residual, an identity. From the quadratic the
;; mean and scale follow, and the route shares nothing with
;; gibbs-posterior above except the model: it calls staged-logpdf, which
;; is already checked against assess, and never looks at a coefficient.
;;
;; So the claim divides cleanly, and each half is checked by the tool that
;; can see it. That the conditional IS Gaussian is proved from the text by
;; gibbs-structure. GIVEN that, which Gaussian it is, is measurable.
;;
;; This is not how a sampler should compute an update — three log-joint
;; evaluations over every child is exactly the re-derivation a conjugate
;; form exists to avoid. It is an instrument.
(define (gibbs-posterior-by-probe st addr choices h)
  (let* ((at-v (lambda (v)
                 (let ((c (map-copy choices)))
                   (map-set! c addr v)
                   (staged-logpdf st c))))
         (c0 (map-ref choices addr))
         (g0 (at-v c0))
         (gp (at-v (+ c0 h)))
         (gm (at-v (- c0 h)))
         ;; g(t) = A t^2 + B t + C about c0
         (a  (/ (- (+ gp gm) (* 2.0 g0)) (* 2.0 h h)))
         (b  (/ (- gp gm) (* 2.0 h))))
    (if (>= a 0.0)
        (error 'gibbs "the log-joint is not concave in this address" addr))
    (cons (- c0 (/ b (* 2.0 a)))
          (sqrt (/ -1.0 (* 2.0 a))))))
