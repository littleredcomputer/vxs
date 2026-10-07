;;; Staging: a model READ rather than run.
;;;
;;; `sample` and `assess` drive a model by running it — the coroutine
;;; suspends at every choice and a stepper decides what the choice is
;;; worth. This file does the other thing: it reads the model's source and
;;; produces a description of its log-joint, which two backends below then
;;; turn into a number (on the VM) or into kernel code (for the device).
;;;
;;; WHY THIS IS NOT A TRACER. A tracer exists because Python cannot hand a
;;; decorator the body of a function as a manipulable datum, so the only
;;; way to learn what a computation does is to run it and watch — which
;;; costs trace memory proportional to what the code DID, in order to
;;; recover what it always SAID. `define-gen` was handed the source at
;;; definition time and kept it (see gf-source), so there is nothing to
;;; reconstruct. The cost here is proportional to the length of the model
;;; text, and the loop below runs once over it whatever N and K are.
;;;
;;; THE TAX. In exchange, a model must say what it means in a subset this
;;; file can read: its data arrive as PARAMETERS rather than as globals,
;;; its helpers are registered kernel functions (define-dual), and its
;;; repeated choices use `batch-i`, which writes the per-element structure
;;; down instead of handing over a finished column. Everything outside
;;; that subset is refused BY NAME rather than miscompiled — a model that
;;; cannot stage is not broken, it just stays on the fiber path, which is
;;; the general case and remains the oracle.

(load "lib/gen.scm")
(load "lib/wgsl.scm")

;;--- the intermediate form ----------------------------------------------
;; A plain datum, deliberately: it is a format, not an object, so
;; something other than the reader below could emit one and both backends
;; would still consume it.
;;
;;   staged  = {:choices ((addr scalar) | (addr batched n) ...)
;;              :buffers ((name . view) ...)
;;              :duals   ((name ((arg type) ...) ret body) ...)
;;              :terms   (term ...)}
;;
;;   term    = (score dist expr)
;;           | (sum-over n idx term)
;;           | (scan-over n idx ((name init step) ...) term)
;;
;;   dist    = (family expr ...)              ; normal, uniform, flip
;;
;;   expr    = <number>
;;           | (choice addr)                  ; a scalar choice's value
;;           | (choice-i addr idx)            ; one element of a batched choice
;;           | (data name idx)                ; one element of a parameter buffer
;;           | (call name expr ...)           ; a registered kernel function
;;           | (+ expr ...) | (- ...) | (* ...) | (/ ...)
;;
;; Every term is a summand of the log-joint, in source order. There is no
;; separate notion of "observed" here: a constrained address and a latent
;; one differ in where their VALUE comes from at evaluation time, not in
;; how they are scored, and keeping that out of the IR is what lets one
;; staged object serve assess-comparison, importance and rejuvenation.

;;--- which distributions may stage --------------------------------------
;; DERIVED, not listed. A family may stage exactly when its score is a
;; DUAL — one body, valid as Scheme and as kernel code — and the score of
;; family F is logpdf-F, the naming regularity lib/dist.scm already
;; declares load-bearing. So the question is asked of the dual registry
;; at the moment it matters, and becoming a dual IS joining: when
;; exponential's score was rewritten as a dual, the hand-kept list this
;; replaces did not notice, which is the failure mode of keeping one
;; fact in two files.
;;
;; The entry is (family logpdf-name parameter-count), the shape the old
;; list held, with the count read off the dual's own parameter list —
;; the score takes the value first, so the family's parameters are the
;; rest, and an arity that cannot drift from the definition it checks.
;;
;; The absences now FALL OUT rather than being maintained: logpdf-gamma
;; and logpdf-beta need lgamma, which WGSL has not got; logpdf-categorical
;; needs an indexed buffer read, the gather this file refuses;
;; logpdf-dirichlet needs both. None of them can be duals, lib/dist.scm
;; says so at each definition, and a model naming one is refused with
;; the family in hand — same refusal, no list to fall behind.
(define (staged-family f)
  (let* ((score (string->symbol
                 (string-append "logpdf-" (symbol->string f))))
         (b     (wgsl-fn-body score)))
    ;; Both halves, which is define-dual exactly: a body the kernel
    ;; compiler can emit AND a procedure the host can evaluate.
    ;; define-gpu alone has no host half; a bare Scheme logpdf has no
    ;; kernel one; neither may stage.
    (and b
         (wgsl-dual score)
         (list f score (- (length (wgsl-body-params b)) 1)))))

;;--- staging context ----------------------------------------------------
;; Accumulators, held in a map because they are genuinely mutable and a
;; map is the mutable thing this language already has.

(define (stage-ctx) {:choices '() :buffers '() :terms '()})

(define (ctx-term! ctx t) (map-set! ctx :terms (cons t (:terms ctx))))

(define (ctx-choice! ctx addr shape)
  (if (assq addr (:choices ctx))
      (error 'stage "an address is used twice" addr))
  (map-set! ctx :choices (cons (cons addr shape) (:choices ctx))))

(define (ctx-buffer! ctx name view)
  (if (not (assq name (:buffers ctx)))
      (map-set! ctx :buffers (cons (cons name view) (:buffers ctx)))))

;;--- the environment ----------------------------------------------------
;; Three kinds of binding, and they behave differently enough that
;; collapsing them would be the first source of a silently wrong answer:
;;
;;   :value  a model PARAMETER, bound to what the call supplied. Numbers
;;           fold into the IR as literals; views become buffers.
;;   :expr   a let-bound name, bound to the IR expression it stands for.
;;   :index  a batch-i or scan-i index, which stands for itself.
;;   :state  a scan-i state component, which also stands for itself — but
;;           is NOT an index, so it cannot be used to read a buffer. That
;;           distinction is the whole of why these are two kinds: indexing
;;           by a state value is a gather, and a gather is a different
;;           primitive with different in-place safety.

(define (env-bind env name kind payload)
  (cons (cons name (cons kind payload)) env))

(define (env-find env name) (assq name env))
(define (binding-kind b) (car (cdr b)))
(define (binding-payload b) (cdr (cdr b)))

;;--- reading an expression ----------------------------------------------

(define (stage-expr e env ctx)
  (cond
    ((number? e) e)
    ((symbol? e) (stage-name e env))
    ((pair? e)   (stage-form e env ctx))
    (else (error 'stage "cannot stage this expression" e))))

(define (stage-name name env)
  (let ((b (env-find env name)))
    (if (not b)
        ;; The most common refusal, so it carries the remedy: staging has
        ;; no eval and cannot read a global, which is exactly why a staged
        ;; model's snapshot cannot go stale.
        (error 'stage
               (string-append
                (symbol->string name)
                ": not bound in the model — a staged model takes what it needs"
                " as a parameter, because staging cannot read a global")
               name)
        (let ((kind (binding-kind b)) (payload (binding-payload b)))
          (cond
            ((eq? kind :expr)  payload)
            ((eq? kind :index) name)
            ((eq? kind :state) name)
            ((eq? kind :value)
             (if (number? payload)
                 payload
                 (error 'stage
                        (string-append (symbol->string name)
                                       ": a buffer parameter is read with view-ref,"
                                       " not used as a number")
                        name)))
            (else (error 'stage "unknown binding kind" name)))))))

(define (stage-form e env ctx)
  (let ((head (car e)))
    (cond
      ((memq head '(let let*)) (stage-let e env ctx))
      ((eq? head 'at)          (stage-at e env ctx))
      ((eq? head 'view-ref)    (stage-view-ref e env ctx))
      ((memq head '(+ - * /))
       (cons head (map (lambda (a) (stage-scalar a env ctx)) (cdr e))))
      ;; A call is a LOOKUP, and the registry is what makes the difference
      ;; between a helper that entered the kernel domain and one that did
      ;; not. Last, so a form above cannot be shadowed by a signature.
      ((and (symbol? head) (wgsl-signature head))
       (cons 'call (cons head (map (lambda (a) (stage-scalar a env ctx)) (cdr e)))))
      ((symbol? head)
       (error 'stage
              (string-append (symbol->string head)
                             ": not a kernel function — this model cannot stage."
                             " Define it with define-dual if it should")
              head))
      (else (error 'stage "cannot stage this form" e)))))

;; A value used in arithmetic must be one number, not a whole batched
;; choice. Refusing here names the address; letting it through would emit
;; a kernel that reads one element and looks plausible.
(define (stage-scalar e env ctx)
  (let ((r (stage-expr e env ctx)))
    (if (and (pair? r) (eq? (car r) 'choice-vector))
        (error 'stage
               "a batched choice is a vector of values, not a number"
               (cadr r))
        r)))

;; `let` stages its inits against the OUTER environment and `let*`
;; against the accumulating one — parallel and sequential, exactly as the
;; model's own Scheme runs them. They used to be one sequential form
;; here, on the theory that a pure body cannot tell the difference; it
;; can, in exactly one case: a binding whose init mentions a name bound
;; earlier in the same list that ALSO exists outside it. Real `let` reads
;; the outer one and the sequential reading took the inner — both run,
;; the log-joints disagree, and the two backends then agree with each
;; other and not with assess, which is the one failure shape a two-path
;; design cannot see on its own.
(define (stage-let e env ctx)
  (let ((sequential? (eq? (car e) 'let*)))
    (let loop ((bs (cadr e)) (bound env))
      (if (null? bs)
          (stage-body (cddr e) bound ctx)
          (let ((name (car (car bs)))
                (init (cadr (car bs))))
            (loop (cdr bs)
                  (env-bind bound name :expr
                            (stage-expr init (if sequential? bound env) ctx))))))))

;; A multi-form body is a sequence whose value is the last form. The
;; earlier ones are staged too — they may contain `at`, which is the only
;; way a form here can matter for anything but its value.
(define (stage-body forms env ctx)
  (if (null? forms)
      (error 'stage "an empty body has no value")
      (let loop ((fs forms))
        (let ((r (stage-expr (car fs) env ctx)))
          (if (null? (cdr fs)) r (loop (cdr fs)))))))

(define (stage-view-ref e env ctx)
  (let* ((name (cadr e))
         (b    (and (symbol? name) (env-find env name))))
    (if (or (not b) (not (eq? (binding-kind b) :value))
            (not (view? (binding-payload b))))
        (error 'stage
               "view-ref must read a buffer the model was given as a parameter"
               name))
    (ctx-buffer! ctx name (binding-payload b))
    (let ((idx (caddr e)))
      (if (not (and (symbol? idx)
                    (let ((ib (env-find env idx)))
                      (and ib (eq? (binding-kind ib) :index)))))
          ;; A computed index would be a gather, which is a different
          ;; primitive with different in-place safety (MANUAL section 6).
          (error 'stage
                 "a staged view-ref is indexed by a batch-i index, nothing else"
                 idx))
      (list 'data name idx))))

;;--- reading a choice ---------------------------------------------------

(define (stage-at e env ctx)
  (let ((addr (cadr e))
        (dist (caddr e)))
    (if (not (keyword? addr))
        (error 'stage "an address must be a keyword" addr))
    (if (and (pair? dist) (eq? (car dist) 'scan-i))
        (stage-scanned addr dist env ctx)
    (if (and (pair? dist) (eq? (car dist) 'batch-i))
        (stage-batched addr dist env ctx)
        (begin
          (ctx-choice! ctx addr '(scalar))
          (ctx-term! ctx (list 'score (stage-dist dist env ctx)
                               (list 'choice addr)))
          (list 'choice addr))))))

;; (batch-i n (j) dist)
(define (stage-batched addr e env ctx)
  (let* ((n    (stage-count (cadr e) env ctx))
         (spec (caddr e))
         (body (cadddr e)))
    (if (or (not (pair? spec)) (not (symbol? (car spec))) (not (null? (cdr spec))))
        (error 'stage "batch-i binds exactly one index" spec))
    (let* ((idx  (car spec))
           (env2 (env-bind env idx :index idx))
           (dist (stage-dist body env2 ctx)))
      (ctx-choice! ctx addr (list 'batched n))
      (ctx-term! ctx (list 'sum-over n idx
                           (list 'score dist (list 'choice-i addr idx))))
      ;; Its value is the whole vector. Named so that arithmetic on it is
      ;; refused rather than silently meaning its first element.
      (list 'choice-vector addr))))

;; (scan-i n (j) ((name init step) ...) dist)
;;
;; THE CEILING IS FIFTEEN STATE COMPONENTS, and it is the device's rather
;; than a shortcut here: fold-i carries ONE accumulator, that accumulator
;; holds the running score as well as the state, and the widest thing WGSL
;; will keep in a register bundle is a 4x4 matrix — sixteen slots, less
;; one for the score.
;;
;; It used to be three, when only vectors were used. A matrix accumulator
;; is a state BUNDLE rather than linear algebra (lib/wgsl.scm exposes no
;; matrix multiply, deliberately), and raising the ceiling this way was the
;; cheapest thing that kept the `point`-terminal question open: an
;; algorithm whose working state fits in the fold has no reason to want
;; per-element scratch, and scratch was the thing pulling toward needing a
;; terminal that is not a point.
;;
;; Refused by name so the boundary is met at the model rather than
;; discovered in a shader.
(define (stage-scanned addr e env ctx)
  (let* ((n      (stage-count (cadr e) env ctx))
         (spec   (caddr e))
         (states (cadddr e))
         (body   (list-ref e 4)))
    (if (or (not (pair? spec)) (not (symbol? (car spec))) (not (null? (cdr spec))))
        (error 'stage "scan-i binds exactly one index" spec))
    (if (null? states)
        (error 'stage "a scan with no state is a batch-i" addr))
    (if (> (length states) 15)
        (error 'stage
               (string-append
                "a scan carries at most fifteen state components — the fold's"
                " accumulator holds the running score too, and the widest"
                " register bundle WGSL has is a 4x4 matrix. Wider state wants"
                " per-element scratch, which is a different mechanism")
               (length states)))
    (for-each
     (lambda (s)
       (if (or (not (pair? s)) (not (symbol? (car s)))
               (not (pair? (cdr s))) (not (pair? (cddr s))) (not (null? (cdddr s))))
           (error 'stage "each scan state must be (name init step)" s)))
     states)
    (let* ((idx   (car spec))
           (names (map car states))
           ;; The initial values are staged in the OUTER environment: they
           ;; are what the state starts AS, so they cannot refer to it.
           (inits (map (lambda (s) (stage-scalar (cadr s) env ctx)) states))
           (env2  (let loop ((ns names) (en (env-bind env idx :index idx)))
                    (if (null? ns) en
                        (loop (cdr ns) (env-bind en (car ns) :state (car ns))))))
           ;; Every step is staged against the SAME environment, so each
           ;; reads the old state. Nothing here threads one into the next.
           (steps (map (lambda (s) (stage-scalar (caddr s) env2 ctx)) states))
           (dist  (stage-dist body env2 ctx)))
      (ctx-choice! ctx addr (list 'batched n))
      (ctx-term! ctx
                 (list 'scan-over n idx
                       (map list names inits steps)
                       (list 'score dist (list 'choice-i addr idx))))
      (list 'choice-vector addr))))

;; A count must be known now, not at run time: it is a loop bound the
;; device compiles in, and fold-i refuses a non-literal for the same
;; reason. Resolving it here is what makes the refusal legible.
(define (stage-count e env ctx)
  (let ((n (stage-expr e env ctx)))
    (if (not (number? n))
        (error 'stage
               "batch-i's count must be known at staging time — pass it as a parameter"
               e))
    n))

(define (stage-dist d env ctx)
  (if (not (pair? d))
      (error 'stage "a choice must be given a distribution" d))
  (let* ((family (car d))
         (entry  (staged-family family)))
    (if (not entry)
        (error 'stage
               (string-append (symbol->string family)
                              ": no score this compiler can place on a device"
                              " — the model stays on the fiber path")
               family))
    (if (not (= (length (cdr d)) (list-ref entry 2)))
        (error 'stage "wrong number of distribution parameters" d))
    (cons family (map (lambda (a) (stage-scalar a env ctx)) (cdr d)))))

;;--- the duals a model reaches ------------------------------------------
;; The IR NAMES its helpers -- (call curve-elem ...) -- and a name is not
;; enough for a backend that has to emit one. So the staged datum carries
;; the helpers this model can reach, each as (name ((arg type) ...) body),
;; which is what define-dual registered in the first place.
;;
;; The body travels as the DATUM, not as compiled text. lib/wgsl.scm's WGSL
;; is one dialect; a reader that has to emit MSL or CUDA needs the readable
;; statement of what the function means rather than one language's answer
;; to it -- the same argument wgsl-bodies already makes for keeping it.
;;
;; TWO WALKS, because the two trees speak different languages. A staged
;; term TAGS its helper calls, (call f a b), while a dual's body is
;; unstaged source where the same helper is a plain application, (f a b).
;; Walking for `call` alone would find the first level and stop there.
;;
;; A DECLARED-ONLY helper has a signature and hand-written WGSL but no
;; readable body, so it cannot appear here and is passed over. That is the
;; omission staged-logpdf already lives with -- it refuses such a call for
;; having no Scheme half -- and the refusal belongs where the body is
;; wanted, not here, since staging accepts the model either way.

;; Every NAME in (call NAME ...), anywhere in a staged form.
(define (ir-call-names form)
  (cond
    ((not (pair? form)) '())
    ((and (eq? (car form) 'call) (pair? (cdr form)) (symbol? (cadr form)))
     (cons (cadr form) (ir-call-names (cddr form))))
    (else (append (ir-call-names (car form)) (ir-call-names (cdr form))))))

;; Every registered helper APPLIED in an unstaged body. Only the registry
;; can tell a helper from an operator here, since both are applications.
(define (body-call-names form)
  (cond
    ((not (pair? form)) '())
    ((and (symbol? (car form)) (wgsl-fn-body (car form)))
     (cons (car form) (body-call-names (cdr form))))
    (else (append (body-call-names (car form)) (body-call-names (cdr form))))))

;; Breadth-first from the terms, following bodies, in first-reached order,
;; so the section is stable enough for a test to compare against.
(define (staged-duals terms)
  (let loop ((pending (ir-call-names terms)) (found '()))
    (cond
      ((null? pending) (reverse found))
      ((assq (car pending) found) (loop (cdr pending) found))
      (else
       (let ((b (wgsl-fn-body (car pending))))
         (if (not b)
             (loop (cdr pending) found)      ; declared-only: see above
             (loop (append (cdr pending) (body-call-names (wgsl-body-expr b)))
                   (cons (list (car pending)
                               (wgsl-body-params b)
                               (wgsl-body-type b)
                               (wgsl-body-expr b))
                         found))))))))

;;--- the entry point ----------------------------------------------------

(define (stage gf)
  (if (not (generative-function? gf))
      (error 'stage "not a generative function" gf))
  (let ((source (gf-source gf))
        (params (gf-params gf))
        (args   (gf-args gf)))
    (if (not source)
        ;; The distinction that matters: unreadable, not empty.
        (error 'stage "this generative function carries no source to read"))
    (if (not (= (length params) (length args)))
        (error 'stage "the model was built with the wrong number of arguments"))
    (let ((ctx (stage-ctx))
          (env (let loop ((ps params) (as args) (env '()))
                 (if (null? ps)
                     env
                     (loop (cdr ps) (cdr as)
                           (env-bind env (car ps) :value (car as)))))))
      (stage-body source env ctx)
      ;; Reversed on the way out: terms are summands of the log-joint and
      ;; float addition is not associative, so source order is the order
      ;; both backends must use if they are to agree.
      {:choices (reverse (:choices ctx))
       :buffers (reverse (:buffers ctx))
       :terms   (reverse (:terms ctx))})))

;;--- the exported IR ----------------------------------------------------
;; What a reader OUTSIDE this VM gets, which is deliberately not the datum
;; `stage` builds. Two forms, because two audiences:
;;
;;   INTERNAL (what stage returns) keeps the family symbol in a score, which
;;   lib/gibbs.scm reads to recognise a conjugate pair -- (eq? (car dist)
;;   'normal) -- and keeps a buffer as (name . view), the view being the live
;;   storage this VM owns.
;;
;;   EXPORTED resolves both. A buffer becomes its name, because `write` prints
;;   a view unreadably. And a score becomes the CALL it already was: both
;;   backends here independently turn (score (normal 0 1.5) v) into
;;   logpdf-normal(v, 0, 1.5) -- staged-score by applying the procedure,
;;   kernel-score by consing the name -- from one naming convention each
;;   re-derives. Doing it once, here, means a reader needs no notion of a
;;   distribution and no copy of that rule.
;;
;; A term is then an expr or a reduction over terms, and `score` stops being
;; a head. Nothing is lost: a :terms entry is a summand of the log-joint by
;; position, which is what made the tag redundant.

(define (export-term t)
  (let ((head (car t)))
    (cond
      ((eq? head 'score)
       (let* ((dist  (cadr t))
              (entry (staged-family (car dist))))
         (cons 'call (cons (cadr entry) (cons (caddr t) (cdr dist))))))
      ((eq? head 'sum-over)
       (list 'sum-over (cadr t) (caddr t) (export-term (cadddr t))))
      ;; A scan's states hold exprs, not terms, so they travel untouched.
      ((eq? head 'scan-over)
       (list 'scan-over (cadr t) (caddr t) (cadddr t)
             (export-term (list-ref t 4))))
      (else (error 'stage "unknown term" t)))))

;; Duals are collected from the LOWERED terms, so the logpdf a score resolves
;; to is found by the same walk as any other call. That is the whole reason
;; the lowering comes first.
(define (staged-export st)
  (let ((terms (map export-term (:terms st))))
    {:choices (:choices st)
     :buffers (map car (:buffers st))
     :duals   (staged-duals terms)
     :terms   terms}))

;;--- strict f32: rounding the VM where the device would ------------------
;; The backends differ in one respect deliberately — f64 here, f32 there —
;; so a comparison across them is a measurement rather than a test. This
;; narrows that gap on purpose: with the flag set, the VM rounds to f32 at
;; every point the IR NAMES an operation, which is where the accumulator
;; lives. It exists to answer one question that a raw measurement cannot —
;; whether a residual difference is the accumulation's width or the two
;; sides disagreeing about what to compute.
;;
;; WHAT IT CANNOT REACH, because the limit bounds the conclusion: a
;; registered helper is a Scheme PROCEDURE, so the arithmetic inside a
;; define-dual body runs in f64 and is rounded only on the way out.
;; logpdf-normal's own subexpressions, and curve-elem's two multiplies,
;; keep an f64 intermediate the device has not got. So what survives this
;; is that residue, plus the few ULP a device's own `log` and `exp` are
;; permitted to differ by — never the accumulation, which the IR owns.
(define staged-f32? #f)

(defmacro (with-staged-f32 . body)
  `(let ((was# staged-f32?))
     (set! staged-f32? #t)
     (unwind-protect (begin ,@body) (set! staged-f32? was#))))

;; One reused four-byte slot. Writing a number into an f32 view and
;; reading it back IS the rounding — there is no separate primitive, and a
;; view is what this language already has for saying a storage width.
(define staged-f32-slot
  (let ((b (make-bytes 4))) (bytes-seal! b) (bytes-view b :f32)))

(define (sf32 x)
  (if staged-f32?
      (begin (view-set! staged-f32-slot 0 x) (view-ref staged-f32-slot 0))
      x))

;;--- backend one: the VM ------------------------------------------------
;; Evaluates the IR in f64, which is what makes it usable as the oracle
;; the device is checked against — and what lets it be compared with
;; `assess` exactly rather than approximately.

(define (staged-logpdf st choices)
  (let loop ((ts (:terms st)) (acc 0.0))
    (if (null? ts)
        acc
        (loop (cdr ts) (sf32 (+ acc (staged-term (car ts) st choices '())))))))

(define (staged-term t st choices idx)
  (let ((head (car t)))
    (cond
      ((eq? head 'score)
       (staged-score (cadr t) (caddr t) st choices idx))
      ((eq? head 'sum-over)
       (let ((n    (cadr t))
             (name (caddr t))
             (body (cadddr t)))
         (let loop ((j 0) (acc 0.0))
           (if (= j n)
               acc
               (loop (+ j 1)
                     (sf32 (+ acc (staged-term body st choices
                                               (cons (cons name j) idx)))))))))
      ((eq? head 'scan-over)
       (let* ((n      (cadr t))
              (name   (caddr t))
              (states (cadddr t))
              (body   (list-ref t 4))
              (names  (map car states)))
         ;; The state travels in the same alist the index does, so a state
         ;; component and a loop index are both just names to the evaluator
         ;; — the difference between them was enforced at staging.
         (let loop ((j 0)
                    (s (map (lambda (st2) (sf32 (staged-value (cadr st2) st choices idx)))
                            states))
                    (acc 0.0))
           (if (= j n)
               acc
               (let ((env (cons (cons name j) (map cons names s))))
                 (loop (+ j 1)
                       ;; Every step reads `env`, which still holds the OLD
                       ;; state — the components update together.
                       (map (lambda (st2) (sf32 (staged-value (caddr st2) st choices env)))
                            states)
                       (sf32 (+ acc (staged-term body st choices env)))))))))
      (else (error 'stage "unknown term" t)))))

;; Dispatched through staged-family rather than a parallel `cond` over
;; the same names. The derivation already says which logpdf each family
;; scores with, and define-dual already registered that logpdf's Scheme
;; half, so a listing here would only be a copy that could fall behind —
;; as the old hand-kept table did, the day exponential became a dual and
;; it did not notice.
(define (staged-score dist value st choices idx)
  (let ((entry (staged-family (car dist)))
        (ps    (map (lambda (a) (staged-value a st choices idx)) (cdr dist)))
        (v     (staged-value value st choices idx)))
    (if (not entry) (error 'stage "unknown family" (car dist)))
    (sf32 (apply (staged-procedure (cadr entry)) (cons v ps)))))

(define (staged-value e st choices idx)
  (cond
    ;; A literal is an f32 literal on the device, so rounding it is part of
    ;; being faithful. A loop index arrives through the symbol branch below
    ;; and stays an exact integer, which is what indexing needs.
    ((number? e) (sf32 e))
    ((symbol? e)
     (let ((b (assq e idx)))
       (if b (cdr b) (error 'stage "unbound index" e))))
    ((pair? e)
     (let ((head (car e)))
       (cond
         ((eq? head 'choice)
          (let ((v (map-ref choices (cadr e))))
            (if (not (number? v))
                (error 'stage "a scalar choice needs a number" (cadr e)))
            (sf32 v)))
         ((eq? head 'choice-i)
          (sf32 (view-ref (map-ref choices (cadr e))
                          (staged-value (caddr e) st choices idx))))
         ((eq? head 'data)
          (sf32 (view-ref (cdr (assq (cadr e) (:buffers st)))
                          (staged-value (caddr e) st choices idx))))
         ((eq? head 'call)
          (sf32 (apply (staged-procedure (cadr e))
                       (map (lambda (a) (staged-value a st choices idx)) (cddr e)))))
         ((memq head '(+ - * /))
          (sf32 (apply (staged-operator head)
                       (map (lambda (a) (staged-value a st choices idx)) (cdr e)))))
         (else (error 'stage "unknown expression" e)))))
    (else (error 'stage "unknown expression" e))))

;; A registered name resolves to the Scheme half of its define-dual. The
;; refusal here draws the distinction that matters: a DECLARED function is
;; hand-written WGSL and has no host meaning at all, so a model calling one
;; can be staged for the device and still cannot be checked against assess
;; — which would defeat the point of having an oracle.
(define (staged-procedure name)
  (let ((b (wgsl-dual name)))
    (if (not b)
        (error 'stage
               (string-append (symbol->string name)
                              ": declared for the device but has no Scheme half,"
                              " so the host cannot evaluate it. Use define-dual"
                              " if the model is to be staged")
               name))
    b))

(define (staged-operator op)
  (cond ((eq? op '+) +) ((eq? op '-) -)
        ((eq? op '*) *) ((eq? op '/) /)
        (else (error 'stage "unknown operator" op))))

;;--- backend two: the device --------------------------------------------
;; Emits an expression in the KERNEL LANGUAGE rather than WGSL text, so
;; that lib/wgsl.scm type-checks it by the same rules as everything else
;; and a mistake here surfaces as a Scheme error rather than a shader log.
;;
;; The free names in the result are the interface a wrangle has to supply:
;; a scalar choice becomes a plain name (a kernel function parameter), and
;; a batched choice or a data buffer becomes a one-argument accessor of
;; the same name. That is the shape lib/wrangle.scm already declares
;; buffer readers in, so wiring it up later adds no new concept.

(define (staged-kernel st)
  (let ((ts (:terms st)))
    (cond
      ((null? ts) 0.0)
      ;; A single term is already the whole log-joint. Wrapping it in a
      ;; one-argument (+ x) would be a sum with nothing to add.
      ((null? (cdr ts)) (kernel-term (car ts) (term-acc (car ts))))
      (else (cons '+ (map (lambda (t) (kernel-term t (term-acc t))) ts))))))

;;--- naming the accumulator --------------------------------------------
;; lib/wgsl.scm already gensyms a fold's accumulator FROM THE NAME the
;; fold-i form gives it — (wgsl-fresh (symbol->string accv)) — and the
;; digits come from a counter that wgsl-compile resets per sub-expression,
;; deliberately, so emitted text depends only on the expression and is
;; comparable by string in the tests.
;;
;; That reset is harmless while the bases DIFFER: two hand-written folds
;; called `acc` and `tot` emit acc_2 and tot_2 and coexist. What broke was
;; this file handing every staged fold the same base, so two staged folds
;; in one kernel were guaranteed to land on the same name — one declaring
;; `var acc_2 : f32` and the other `var acc_2 : vec4<f32>`, with the first
;; reader silently getting the second fold's value.
;;
;; So the accumulator is named for WHAT IT ACCUMULATES. Addresses are
;; unique — staging refuses a repeat — so this is distinct by construction
;; rather than by a counter, and the emitted `acc_ys_2` says which term it
;; belongs to instead of leaving a reader to count folds. A nested
;; reduction deepens the name rather than reusing it.
(define (term-address t)
  (cond
    ((eq? (car t) 'score)
     (let ((v (caddr t)))
       (if (and (pair? v) (memq (car v) '(choice choice-i))) (cadr v) #f)))
    ((eq? (car t) 'sum-over)  (term-address (cadddr t)))
    ((eq? (car t) 'scan-over) (term-address (list-ref t 4)))
    (else #f)))

(define (term-acc t)
  (let ((a (term-address t)))
    (if a
        (string->symbol (string-append "acc-" (keyword->string a)))
        'acc)))

(define (acc-deeper name)
  (string->symbol (string-append (symbol->string name) "-i")))

(define (kernel-term t acc)
  (let ((head (car t)))
    (cond
      ((eq? head 'score) (kernel-score (cadr t) (caddr t)))
      ((eq? head 'sum-over)
       ;; The device's bounded fold. Its index is :u32 — an address, not a
       ;; quantity — which is why nothing below ever uses it as a number.
       (let ((body (kernel-term (cadddr t) (acc-deeper acc))))
         (if staged-blocked?
             (kernel-blocked-sum (cadr t) (caddr t) body acc)
             (list 'fold-i (cadr t) 0.0 (list (caddr t) acc)
                   (list '+ acc body)))))
      ((eq? head 'scan-over) (kernel-scan t acc))
      (else (error 'stage "unknown term" t)))))

;;--- blocked summation, for the likelihood fold -------------------------
;; A bare f32 accumulator grows toward the total while each new addend
;; stays small, so the addend's low bits fall below the accumulator's ULP
;; and are rounded away — once per term, with the error growing as O(n).
;;
;; THREE WAYS TO FIX A FLOAT SUM, AND ONLY ONE IS AVAILABLE HERE.
;;
;;   A wider accumulator is the obvious answer and does not exist: WGSL
;;   has no f64 and Metal has no double, so there is nothing to widen to.
;;
;;   Compensated summation (Kahan, and the double-single pair built on the
;;   same two-sum) recovers the dropped bits by carrying them forward. It
;;   was implemented here and MEASURED: the host confirmed the
;;   compensation changes 311 of 512 particles and cuts summation error
;;   3x, and the device returned results bit-identical to the plain sum
;;   for all 512. The compiler deleted it, as it is entitled to —
;;   (t - sum) - y is algebraically zero. Removed rather than kept as dead
;;   code that looks alive.
;;
;;   Reassociation survives, because there is no identity to cancel. That
;;   is what this is.
;;
;; FOUR partial sums in a vec4, rotating which lane receives each term:
;;
;;   acc = vec4(acc.y, acc.z, acc.w, acc.x + term)
;;
;; so lane k accumulates every fourth term and the dependent add chain per
;; lane is n/4 rather than n. The rotation is real data movement and the
;; result is a genuine reassociation, so there is no identity to cancel —
;; a compiler that reassociates is doing what this asks for rather than
;; undoing it. It also needs no divisibility: any n rotates.
;;
;; MEASURED ON A DEVICE, curve model at n = 32, K = 512, against the f64
;; oracle: mean error 2.59 ulp -> 0.79 ulp, and the bias 2.01 ulp -> 0.12
;; ulp. Sub-ulp and unbiased, which is the representable floor.
;;
;; OFF BY DEFAULT, deliberately. At n = 32 the plain sum's error is
;; already far below anything that reads a log-joint: the bias is the
;; benign kind for Metropolis-Hastings, which accepts on a DIFFERENCE of
;; log-joints at nearby parameters and cancels a common bias, and the
;; residual noise is orders of magnitude under any sane proposal scale.
;; What earns this its place in the file is that the error grows with n —
;; a likelihood over ten thousand points is 10,000 roundings deep rather
;; than 2,500, and there the argument changes. Reach for it then.
;;
;; A note on cost: four accumulators instead of one, so four registers
;; where a scan nested inside a fold may already be using the bundle
;; machinery. The four chains are independent, so this may well be the
;; faster form too — unmeasured.
(define staged-blocked? #f)

(defmacro (with-staged-blocked . body)
  `(let ((was# staged-blocked?))
     (set! staged-blocked? #t)
     (unwind-protect (begin ,@body) (set! staged-blocked? was#))))

;; The fold is let-bound because its four lanes must be mentioned four
;; times, and mentioning the fold itself would emit the loop four times.
(define (kernel-blocked-sum n idx term acc)
  (let ((bsum (string->symbol (string-append (symbol->string acc) "-lanes"))))
    (list 'let
          (list (list bsum
                      (list 'fold-i n (list 'vec4 0.0 0.0 0.0 0.0) (list idx acc)
                            (list 'vec4 (list 'swizzle acc 'y)
                                        (list 'swizzle acc 'z)
                                        (list 'swizzle acc 'w)
                                        (list '+ (list 'swizzle acc 'x) term)))))
          (list '+ (list 'swizzle bsum 'x) (list 'swizzle bsum 'y)
                   (list 'swizzle bsum 'z) (list 'swizzle bsum 'w)))))

;; A scan is the same bounded fold with a WIDER accumulator: the state
;; components and the running score, packed into one vector because fold-i
;; carries exactly one value. Unpacked at the top of each pass and repacked
;; at the bottom.
;;
;; The repack is where the simultaneous update becomes real. Every step
;; expression is emitted against the names bound from the OLD accumulator,
;; and the new vector is built in one constructor, so there is no moment at
;; which one component can see another's new value.
(define kernel-lanes '(x y z w))

;; (capacity constructor matrix?), narrowest first. A vector up to four
;; slots, then a matrix — which is a state BUNDLE here rather than linear
;; algebra, and is why lib/wgsl.scm exposes no matrix multiply.
;;
;; Columns are always four components, so element k lives at column k/4,
;; row k%4 — which is the whole of the addressing below.
(define kernel-bundles
  '((2 vec2 #f) (3 vec3 #f) (4 vec4 #f)
    (8 mat2x4 #t) (12 mat3x4 #t) (16 mat4x4 #t)))

(define (kernel-bundle n)
  (let loop ((bs kernel-bundles))
    (cond ((null? bs) #f)
          ((<= n (car (car bs))) (car bs))
          (else (loop (cdr bs))))))

(define (kernel-slot acc k matrix?)
  (if matrix?
      (list 'swizzle (list 'mat-col acc (quotient k 4))
            (list-ref kernel-lanes (remainder k 4)))
      (list 'swizzle acc (list-ref kernel-lanes k))))

(define (kernel-zeros n)
  (let loop ((i 0) (acc '())) (if (= i n) acc (loop (+ i 1) (cons 0.0 acc)))))

(define (kernel-scan t acc)
  (let* ((n      (cadr t))
         (idx    (caddr t))
         (states (cadddr t))
         (body   (list-ref t 4))
         (names  (map car states))
         (width  (+ (length states) 1))
         (bundle (kernel-bundle width))
         (score  (string->symbol (string-append (symbol->string acc) "-score"))))
    (if (not bundle)
        (error 'stage "a scan carries at most fifteen state components" t))
    ;; The fold's VALUE is the whole packed accumulator, but a term
    ;; contributes only its score — so the last slot comes back out. The
    ;; state was scaffolding for producing it and does not survive.
    (kernel-slot (kernel-scan-fold n idx states body names bundle score acc)
                 (- width 1) (caddr bundle))))

(define (kernel-scan-fold n idx states body names bundle score acc)
  (let* ((cap     (car bundle))
         (ctor    (cadr bundle))
         (matrix? (caddr bundle))
         (all     (append names (list score))))
    ;; A bundle is filled exactly: a vector of the width needed, or a
    ;; matrix padded to its full component count. The padding is dead
    ;; weight in registers and costs nothing measurable.
    (define (filled vals) (append vals (kernel-zeros (- cap (length vals)))))
    (list 'fold-i n
          (cons ctor (filled (append (map (lambda (s) (kernel-value (cadr s))) states)
                                     (list 0.0))))
          (list idx acc)
          (list 'let
                ;; `binds`, not `acc`: this is the list of let-bindings being
                ;; built, and calling it acc would shadow the accumulator's
                ;; NAME, which is now a parameter rather than a constant.
                (let loop ((ns all) (k 0) (binds '()))
                  (if (null? ns)
                      (reverse binds)
                      (loop (cdr ns) (+ k 1)
                            (cons (list (car ns) (kernel-slot acc k matrix?)) binds))))
                (cons ctor
                      (filled
                       (append (map (lambda (s) (kernel-value (caddr s))) states)
                               (list (list '+ score
                                           (kernel-term body (acc-deeper acc)))))))))))

(define (kernel-score dist value)
  (let ((entry (staged-family (car dist))))
    (cons (cadr entry)
          (cons (kernel-value value)
                (map kernel-value (cdr dist))))))

(define (kernel-value e)
  (cond
    ((number? e) e)
    ((symbol? e) e)
    ((pair? e)
     (let ((head (car e)))
       (cond
         ((eq? head 'choice)   (keyword->name (cadr e)))
         ((eq? head 'choice-i) (list (keyword->name (cadr e)) (caddr e)))
         ((eq? head 'data)     (list (cadr e) (caddr e)))
         ((eq? head 'call)     (cons (cadr e) (map kernel-value (cddr e))))
         ((memq head '(+ - * /)) (cons head (map kernel-value (cdr e))))
         (else (error 'stage "unknown expression" e)))))
    (else (error 'stage "unknown expression" e))))

(define (keyword->name k) (string->symbol (keyword->string k)))
