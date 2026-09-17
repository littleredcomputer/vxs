;;; Random-walk Metropolis-Hastings over a staged log-joint.
;;;
;;; The rejuvenation half of the SMC pipeline, and the route to it that
;;; needs no conjugacy. lib/gibbs.scm asks whether a model's conditional
;;; has a closed form and refuses when it has not; MH asks nothing of the
;;; model but its log-joint, which lib/stage.scm already produces for the
;;; VM and for a device. So every model that stages can be rejuvenated,
;;; whether or not anything can derive an update for it.
;;;
;;; WHY THE LOG-JOINT BECOMES A FUNCTION. An accept ratio needs the
;;; log-joint at TWO parameter values, and inlining the staged expression
;;; twice would put its likelihood fold in the kernel twice — two
;;; reductions, and twice the work for an answer that differs only in
;;; three arguments. wgsl-define-fn! takes the staged expression as a body
;;; and the scalar choices as parameters, so the fold is emitted once
;;; inside `fn logjoint(...)` and the kernel calls it twice.
;;;
;;; That the staged kernel's free names ARE the scalar choices is what
;;; makes this free: the expression is already a function of them in
;;; everything but name.
;;;
;;; WHY THE ERROR MEASUREMENT LICENSES THIS. The device's log-joint
;;; carries a small systematic bias against an f64 evaluation (MANUAL
;;; section 6). An accept ratio is a DIFFERENCE of log-joints at nearby
;;; parameter values, so a bias common to both largely cancels — which is
;;; the benign half of that measurement, and the half this depends on.
;;;
;;; SWEEPS ARE SUBSTEPS. gpu-wrangle!'s substep count runs the kernel N
;;; times in one submit and hands each run its own value of `step`, which
;;; the preamble gives to rng_init as the stream index. Sweeping any other
;;; way would replay the identical proposals — the exact hazard MANUAL
;;; section 4 warns about, and the one that would make a chain look busy
;;; while going nowhere.

(load "lib/stage.scm")

;;--- what MH moves ------------------------------------------------------
;; The SCALAR choices, in source order. A batched choice is not a latent
;; here: it is what the latents are scored against, and staging already
;; refuses an unconstrained one.

(define (staged-scalar-addrs st)
  (let loop ((cs (:choices st)) (acc '()))
    (cond ((null? cs) (reverse acc))
          ((eq? (cdr (car cs)) 'scalar) (loop (cdr cs) (cons (car (car cs)) acc)))
          (else (loop (cdr cs) acc)))))

;; The names those addresses carry in the kernel — the same mapping
;; lib/stage.scm uses when it emits them, because they have to agree.
(define (staged-scalar-names st)
  (map (lambda (a) (string->symbol (keyword->string a))) (staged-scalar-addrs st)))

(define (staged-scalar-params st)
  (map (lambda (n) (list n :f32)) (staged-scalar-names st)))

;;--- the log-joint as a callable kernel function -----------------------
;; Returns the parameter names, so a caller need not re-derive the order
;; it must pass arguments in.

(define (staged-logjoint-fn! st name)
  (let ((ps (staged-scalar-params st)))
    (if (null? ps)
        (error 'mh "a model with no scalar choice has nothing to rejuvenate"))
    (wgsl-define-fn! name ps (staged-kernel st))
    (map car ps)))

;;--- one sweep, as a wrangle body --------------------------------------
;; (staged-mh-body st fname step accept) -> an expression ending in point.
;;
;; `step` and `accept` are left as the caller's: step is any kernel
;; expression, so it may be a literal or a live wrangle parameter, and
;; accept names a declared attribute to record the decision in. An
;; acceptance rate is the one diagnostic that says whether a step size is
;; sane, and it costs one attribute.
;;
;; The `let` wraps the TERMINAL rather than sitting inside each attribute
;; expression, and that is not a style choice. The terminal compiles each
;; of its attribute arguments separately, so a proposal computed inside
;; them would be computed once per attribute — and since a draw is
;; stateful, each attribute would get a DIFFERENT proposal and the point
;; would fly apart while looking plausible. One let, one proposal, one
;; decision, N writes of it.
;;
;; A JOINT proposal: every scalar choice moves at once and one accept
;; decision covers them all. Componentwise would mix better on a
;; correlated posterior — curve coefficients are strongly correlated — and
;; costs one call per address per sweep rather than two calls, which the
;; function above makes affordable. Not done here because a joint chain is
;; the smaller correct thing and the demo will say whether it mixes.

(define (mh-name prefix n)
  (string->symbol (string-append prefix (symbol->string n))))

(define (staged-mh-body st fname step accept)
  (let* ((names (staged-scalar-names st))
         (props (map (lambda (n) (mh-name "mh-p-" n)) names))
         (noise (map (lambda (n) (mh-name "mh-n-" n)) names)))
    ;; Built by hand rather than with a multi-list map, so the CONSUMPTION
    ;; ORDER is written down where it can be read: one normal per address
    ;; in source order, then one uniform. The host twin below draws in the
    ;; same order, which is the whole of why the two can be compared.
    (let build ((ns names) (ps props) (vs noise)
                (draws '()) (offers '()) (writes '()))
      (if (not (null? ns))
          (build (cdr ns) (cdr ps) (cdr vs)
                 (cons (list (car vs) (list 'random-normal 0.0 step)) draws)
                 (cons (list (car ps) (list '+ (car ns) (car vs))) offers)
                 (cons (list (car ns) (list 'if 'mh-take (car ps) (car ns))) writes))
          (list 'let*
                (append (list (list 'mh-cur (cons fname names)))
                        (reverse draws)
                        (reverse offers)
                        (list (list 'mh-prop (cons fname props))
                              (list 'mh-u (list 'random-uniform 0.0 1.0))
                              ;; log u < log alpha, rather than u < exp(log
                              ;; alpha): the exponential overflows to zero
                              ;; for a strongly rejected proposal and the
                              ;; comparison then depends on how far past
                              ;; the floor it went.
                              (list 'mh-take
                                    (list '< (list 'log 'mh-u)
                                          (list '- 'mh-prop 'mh-cur)))))
                (append (list 'point 'position 'pscale 'colour)
                        (reverse writes)
                        (list (list accept (list 'if 'mh-take 1.0 0.0)))))))))

;;--- the host twin -----------------------------------------------------
;; The same sweep on the VM, and the reason it exists is the reason
;; everything here has one: it is the oracle. It draws in the same order
;; from the same counter-based generator, so a particle on the device and
;; a particle here make the SAME proposal and the SAME decision, to f32.
;;
;; It is also how MH is checked at all. A chain cannot be compared against
;; an expected value sample by sample — but on a model whose posterior is
;; known in closed form, what the chain CONVERGES to can be.
(define (mh-sweep st choices step r)
  (let* ((names (staged-scalar-addrs st))
         (cur   (staged-logpdf st choices))
         (offer (map-copy choices)))
    (let draw ((ns names))
      (if (not (null? ns))
          (begin
            (map-set! offer (car ns)
                      (+ (map-ref choices (car ns)) (random-normal r 0.0 step)))
            (draw (cdr ns)))))
    (let ((prop (staged-logpdf st offer))
          (u    (random-uniform r 0.0 1.0)))
      (if (< (log u) (- prop cur))
          (cons offer #t)
          (cons choices #f)))))

;; N sweeps, reporting what fraction were accepted. The rate is the
;; diagnostic: near zero means the step is too big to ever land, near one
;; means it is too small to go anywhere, and neither shows up in the
;; samples as obviously as it does here.
(define (mh-run st choices step r n)
  (let loop ((i 0) (ch choices) (taken 0))
    (if (= i n)
        (cons ch (/ (exact->inexact taken) (exact->inexact n)))
        (let ((r2 (mh-sweep st ch step r)))
          (loop (+ i 1) (car r2) (if (cdr r2) (+ taken 1) taken))))))
