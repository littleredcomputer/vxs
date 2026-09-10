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
;; This list IS the answer to "can this model go on a device", and it is
;; short for a reason that is not laziness: lib/stat.wgsl has scores for
;; these three and no others.
;;
;; The absences are structural, not pending work. logpdf-gamma and
;; logpdf-beta need lgamma, which WGSL does not have and cannot cheaply
;; get; lib/dist.scm says so at both definitions. logpdf-exponential is a
;; near miss — the host has it, the device has random_exponential but no
;; score to go with it, so it is one define-dual away rather than one
;; approximation away.
;;
;; Keeping the map here rather than in a comment is the point: a model
;; using beta is refused by name at staging, instead of emitting a call to
;; a function the device does not define.
(define staged-families
  ;; (family logpdf-name parameter-count)
  '((normal  logpdf-normal  2)
    (uniform logpdf-uniform 2)
    (flip    logpdf-flip    1)))

(define (staged-family f) (assq f staged-families))

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

(define (stage-let e env ctx)
  (let loop ((bs (cadr e)) (env env))
    (if (null? bs)
        ;; let* and let are the same form here because a staged body has
        ;; no side effects: nothing can observe the difference between
        ;; binding in sequence and binding in parallel except a name that
        ;; shadows, and that reads the same either way.
        (stage-body (cddr e) env ctx)
        (let ((name (car (car bs)))
              (init (cadr (car bs))))
          (loop (cdr bs)
                (env-bind env name :expr (stage-expr init env ctx)))))))

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
          (ctx-choice! ctx addr 'scalar)
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
;; THE CEILING IS THREE STATE COMPONENTS, and it is a property of the
;; device rather than a shortcut here: fold-i carries ONE accumulator, that
;; accumulator has to hold the running score as well as the state, and a
;; WGSL vector stops at four components. Wide state wants the per-element
;; scratch-attribute path instead (MANUAL section 6), which is a different
;; mechanism and not an extension of this one. Refused by name here so that
;; the boundary is met at the model rather than discovered in a shader.
(define (stage-scanned addr e env ctx)
  (let* ((n      (stage-count (cadr e) env ctx))
         (spec   (caddr e))
         (states (cadddr e))
         (body   (list-ref e 4)))
    (if (or (not (pair? spec)) (not (symbol? (car spec))) (not (null? (cdr spec))))
        (error 'stage "scan-i binds exactly one index" spec))
    (if (null? states)
        (error 'stage "a scan with no state is a batch-i" addr))
    (if (> (length states) 3)
        (error 'stage
               (string-append
                "a scan carries at most three state components — the fold's"
                " accumulator holds the score too, and a device vector stops"
                " at four. Wide state wants scratch attributes")
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

;;--- backend one: the VM ------------------------------------------------
;; Evaluates the IR in f64, which is what makes it usable as the oracle
;; the device is checked against — and what lets it be compared with
;; `assess` exactly rather than approximately.

(define (staged-logpdf st choices)
  (let loop ((ts (:terms st)) (acc 0.0))
    (if (null? ts)
        acc
        (loop (cdr ts) (+ acc (staged-term (car ts) st choices '()))))))

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
                     (+ acc (staged-term body st choices
                                         (cons (cons name j) idx))))))))
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
                    (s (map (lambda (st2) (staged-value (cadr st2) st choices idx))
                            states))
                    (acc 0.0))
           (if (= j n)
               acc
               (let ((env (cons (cons name j) (map cons names s))))
                 (loop (+ j 1)
                       ;; Every step reads `env`, which still holds the OLD
                       ;; state — the components update together.
                       (map (lambda (st2) (staged-value (caddr st2) st choices env))
                            states)
                       (+ acc (staged-term body st choices env))))))))
      (else (error 'stage "unknown term" t)))))

(define (staged-score dist value st choices idx)
  (let* ((family (car dist))
         (ps     (map (lambda (a) (staged-value a st choices idx)) (cdr dist)))
         (v      (staged-value value st choices idx)))
    (cond
      ((eq? family 'normal)  (logpdf-normal  v (car ps) (cadr ps)))
      ((eq? family 'uniform) (logpdf-uniform v (car ps) (cadr ps)))
      ((eq? family 'flip)    (logpdf-flip    v (car ps)))
      (else (error 'stage "unknown family" family)))))

(define (staged-value e st choices idx)
  (cond
    ((number? e) e)
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
            v))
         ((eq? head 'choice-i)
          (view-ref (map-ref choices (cadr e))
                    (staged-value (caddr e) st choices idx)))
         ((eq? head 'data)
          (view-ref (cdr (assq (cadr e) (:buffers st)))
                    (staged-value (caddr e) st choices idx)))
         ((eq? head 'call)
          (apply (staged-procedure (cadr e))
                 (map (lambda (a) (staged-value a st choices idx)) (cddr e))))
         ((memq head '(+ - * /))
          (apply (staged-operator head)
                 (map (lambda (a) (staged-value a st choices idx)) (cdr e))))
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
    (cdr b)))

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
      ((null? (cdr ts)) (kernel-term (car ts)))
      (else (cons '+ (map (lambda (t) (kernel-term t)) ts))))))

(define (kernel-term t)
  (let ((head (car t)))
    (cond
      ((eq? head 'score) (kernel-score (cadr t) (caddr t)))
      ((eq? head 'sum-over)
       ;; The device's bounded fold. Its index is :u32 — an address, not a
       ;; quantity — which is why nothing below ever uses it as a number.
       (list 'fold-i (cadr t) 0.0 (list (caddr t) 'acc)
             (list '+ 'acc (kernel-term (cadddr t)))))
      ((eq? head 'scan-over) (kernel-scan t))
      (else (error 'stage "unknown term" t)))))

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

(define (kernel-scan t)
  (let* ((n      (cadr t))
         (idx    (caddr t))
         (states (cadddr t))
         (body   (list-ref t 4))
         (names  (map car states))
         (width  (+ (length states) 1))
         (ctor   (list-ref '(#f #f vec2 vec3 vec4) width))
         (score  'acc-score))
    (if (not ctor) (error 'stage "a scan needs one to three state components" t))
    ;; The fold's VALUE is the whole packed accumulator, but a term
    ;; contributes only its score — so the last lane comes back out. The
    ;; state was scaffolding for producing it and does not survive.
    (list 'swizzle
          (kernel-scan-fold n idx states body names ctor score)
          (list-ref kernel-lanes (- width 1)))))

(define (kernel-scan-fold n idx states body names ctor score)
  (let ((all (append names (list score))))
    (list 'fold-i n
          (cons ctor (append (map (lambda (s) (kernel-value (cadr s))) states)
                             (list 0.0)))
          (list idx 'acc)
          (list 'let
                (let loop ((ns all) (ls kernel-lanes) (acc '()))
                  (if (null? ns)
                      (reverse acc)
                      (loop (cdr ns) (cdr ls)
                            (cons (list (car ns) (list 'swizzle 'acc (car ls))) acc))))
                (cons ctor
                      (append (map (lambda (s) (kernel-value (caddr s))) states)
                              (list (list '+ score (kernel-term body)))))))))

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
