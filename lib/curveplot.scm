;;; The posterior of a curve family, drawn by the GPU.
;;;
;;; ONE PROCEDURE, THREE JOBS. A curve family is a `define-dual` from
;;; (x, parameters...) to a value. The model scores with it, the VM
;;; evaluates it as the oracle, and this file draws with it — so changing
;;; the body, or the NUMBER of parameters, changes all three. There is no
;;; second copy of the curve anywhere, which is the whole claim.
;;;
;;; Contrast lib/fitplot.scm, which draws the same posterior on the CPU
;;; and has the quadratic written into it by hand. That is not a criticism
;;; of it — it is what a plotter has to do when it cannot be told what the
;;; curve is — and it is exactly the binding this removes.
;;;
;;; HOW THE PICTURE IS MADE. Not by drawing N curves and blending them:
;;; per pixel, the kernel loops over the particles and accumulates
;;; coverage, so the image IS the posterior predictive density, computed
;;; rather than composited. No binning decision, no geometry, and a stroke
;;; whose width is uniform because the distance is normalised by the
;;; screen-space gradient (MANUAL section 6) and therefore measured in
;;; pixels rather than in plot units.

(load "lib/gpu.scm")
(load "lib/stage.scm")

;;--- what the kernel is told --------------------------------------------
;; One shared region per parameter, NDRAW entries each, plus the truth.
;; The layout is derived from the staged model's choices, so a model that
;; gains a parameter gains a region and a kernel argument with it.

(define (curveplot-scalars st)
  ;; The scalar choices, in source order — which is also the order the
  ;; curve's parameters were written in, since both come from the model.
  (let loop ((cs (:choices st)) (acc '()))
    (cond ((null? cs) (reverse acc))
          ((eq? (cdr (car cs)) 'scalar) (loop (cdr cs) (cons (car (car cs)) acc)))
          (else (loop (cdr cs) acc)))))

(define (curveplot-region addr) (string->symbol (keyword->string addr)))

;; The batched choice — its address and how many of it there are. That is
;; the observation count, and the model already knows it, so the plot does
;; not have to be told.
(define (curveplot-batched st)
  (let loop ((cs (:choices st)))
    (cond ((null? cs)
           (error 'plot-posterior! "the model has no batched choice to plot"))
          ((and (pair? (cdr (car cs))) (eq? (car (cdr (car cs))) 'batched))
           (cons (car (car cs)) (cadr (cdr (car cs)))))
          (else (loop (cdr cs))))))

;; Which buffer the model read its x values from. It said so when it wrote
;; (view-ref xs j), and one buffer is the shape this plot understands —
;; more than one is refused rather than guessed at.
(define (curveplot-xs st)
  (let ((bs (:buffers st)))
    (if (not (= (length bs) 1))
        (error 'plot-posterior!
               "this plot needs the model to read exactly one buffer for x"
               (map car bs)))
    (cdr (car bs))))

;;--- the kernel ---------------------------------------------------------
;; Built from the parameter list rather than written out, so it follows
;; the model. `fn` is the curve's name; the call is (fn x p1 p2 ...) with
;; the parameters read from this particle's slot in each region.

(define (curveplot-kernel fn params ndraw npts x0 x1 y0 y1 ink)
  (let* ((args (map (lambda (p)
                      (list (string->symbol
                             (string-append "shared-" (keyword->string p)))
                            'k))
                    params))
         ;; The residual field, in plot coordinates: where this particle's
         ;; curve sits relative to this pixel.
         (g (list '- (cons fn (cons 'px args)) 'py)))
    `(let* ((px (+ ,x0 (* ,(- x1 x0) (swizzle uv x))))
            (py (+ ,y1 (* ,(- y0 y1) (swizzle uv y))))
            (ink (fold-i ,ndraw 0.0 (k acc)
                   (let* ((g ,g)
                          ;; Distance in PIXELS. dpdy picks up the vertical
                          ;; scale on its own, so the axes need not share one.
                          (d (/ (abs g) (length (vec2 (dpdx g) (dpdy g))))))
                     (+ acc (* ,ink (- 1.0 (smoothstep 0.0 1.5 d)))))))
            ;; The truth, drawn once in amber, from the same curve.
            ;; (u32 0), not 0: every kernel literal is an f32 by design, and
            ;; a region index is an address rather than a quantity.
            (tg ,(list '- (cons fn (cons 'px (map (lambda (p)
                                                    (list (string->symbol
                                                           (string-append "shared-truth-"
                                                                          (keyword->string p)))
                                                          '(u32 0)))
                                                  params)))
                       'py))
            (td (/ (abs tg) (length (vec2 (dpdx tg) (dpdy tg)))))
            (tr (- 1.0 (smoothstep 0.0 1.5 td)))
            ;; The observations. A disc wants a distance in PIXELS, and
            ;; the two axes do not share a scale — so the offset is
            ;; converted with res rather than measured in plot units,
            ;; which would draw ellipses. No derivative needed: the
            ;; conversion is exact and the spans are known here.
            (dots (fold-i ,npts 0.0 (m acc)
                    (let* ((sx (* (- px (shared-obs-x m))
                                  (/ (swizzle res x) ,(- x1 x0))))
                           (sy (* (- py (shared-obs-y m))
                                  (/ (swizzle res y) ,(- y1 y0))))
                           (dd (length (vec2 sx sy))))
                      ;; max, not +: two dots that overlap should not be
                      ;; brighter than one.
                      (max acc (- 1.0 (smoothstep 2.5 4.0 dd)))))))
       (vec3 (+ (* 0.10 ink) (* 0.95 tr) dots)
             (+ (* 0.85 ink) (* 0.70 tr) dots)
             (+ (* 0.80 ink) (* 0.15 tr) dots)))))

;;--- the whole picture --------------------------------------------------
;; (plot-posterior! gf curve-name cols picks truth ...) -> a future
;;
;; `cols` is what `importance` returned and `picks` an :i32 view of
;; particle indices resampled by weight — duplicates included, because a
;; duplicate is what makes a concentrated posterior LOOK concentrated.

(define (plot-posterior! gf fn cols picks truth ys . opts)
  (let* ((st     (stage gf))
         (params (curveplot-scalars st))
         (batch  (curveplot-batched st))
         (npts   (cdr batch))
         (xs     (curveplot-xs st))
         (ndraw  (view-length picks))
         (pad    (if (pair? opts) (car opts) 0.15))
         (ink    (if (and (pair? opts) (pair? (cdr opts))) (cadr opts) 0.07)))
    (if (< (view-length ys) npts)
        (error 'plot-posterior!
               "fewer observations than the model's batched choice expects"
               (list (view-length ys) npts)))
    (if (not (= (length params) (length truth)))
        (error 'plot-posterior!
               "the model's scalar choices and the truth must correspond"
               (list (length params) (length truth))))
    ;; Two regions per parameter: the resampled particles, and the one
    ;; true value. The truth rides in the same buffer so the kernel reads
    ;; it exactly as it reads a particle — one accessor pattern, not two.
    (shared-layout!
     (append (map (lambda (p) (list (curveplot-region p) ndraw)) params)
             (map (lambda (p)
                    (list (string->symbol
                           (string-append "truth-" (keyword->string p))) 1))
                  params)
             ;; The observations, so the picture shows what it is fitting TO.
             (list (list 'obs-x npts) (list 'obs-y npts))))
    (let* ((bytes (make-shared))
           (v     (shared-view bytes)))
      (for-each
       (lambda (p)
         (let ((col (map-ref cols p))
               (off (shared-offset (curveplot-region p))))
           (let loop ((i 0))
             (if (< i ndraw)
                 (begin (view-set! v (+ off i) (view-ref col (view-ref picks i)))
                        (loop (+ i 1)))))))
       params)
      (let loop ((ps params) (ts truth))
        (if (pair? ps)
            (begin
              (view-set! v (shared-offset
                            (string->symbol
                             (string-append "truth-" (keyword->string (car ps)))))
                         (car ts))
              (loop (cdr ps) (cdr ts)))))
      (let ((ox (shared-offset 'obs-x)) (oy (shared-offset 'obs-y)))
        (let loop ((j 0))
          (if (< j npts)
              (begin (view-set! v (+ ox j) (view-ref xs j))
                     (view-set! v (+ oy j) (view-ref ys j))
                     (loop (+ j 1))))))
      ;; The window frames the PARTICLES, so it follows the curve family
      ;; rather than assuming one. A curve that leaves the frame is still
      ;; the news; a frame that cannot contain the family is only a bug.
      (let* ((b  (curveplot-bounds fn v params ndraw xs ys npts))
             (x0 (car b)) (x1 (cadr b)) (y0 (caddr b)) (y1 (cadddr b))
             (dy (* pad (- y1 y0))))
        (run-kernel-loop
         (shadertoy (curveplot-kernel fn params ndraw npts
                                      x0 x1 (- y0 dy) (+ y1 dy) ink))
         "vxs-gpu-canvas" bytes)))))

;; Where the particles actually go, sampled across the x range. Cheap —
;; NDRAW by a handful of probes — and it is what lets the same plotter
;; frame a quadratic and a sine without being told which it has.
(define (curveplot-bounds fn v params ndraw xs ys npts)
  (let* ((span (lambda (view n)
                 (let loop ((j 0) (lo 1e30) (hi -1e30))
                   (if (= j n)
                       (cons lo hi)
                       (let ((z (view-ref view j)))
                         (loop (+ j 1) (min lo z) (max hi z)))))))
         ;; x frames the observations. They are where the model was given
         ;; something to believe, so they are what the picture is about.
         (xr (span xs npts))
         (xp (* 0.1 (- (cdr xr) (car xr))))
         (x0 (- (car xr) xp))
         (x1 (+ (cdr xr) xp))
         (yr (span ys npts))
         (probes 9))
    ;; y frames the particles AND the observations: a datum outside the
    ;; fan is precisely the news, so it must not be cropped out of it.
    (let loop ((i 0) (lo (car yr)) (hi (cdr yr)))
      (if (= i ndraw)
          (list x0 x1 lo hi)
          (let probe ((s 0) (lo lo) (hi hi))
            (if (> s probes)
                (loop (+ i 1) lo hi)
                (let* ((x (+ x0 (* (/ (- x1 x0) probes) s)))
                       (y (curveplot-eval fn v params i x)))
                  (probe (+ s 1) (min lo y) (max hi y)))))))))

;; The curve, evaluated on the HOST for particle i — the same arithmetic
;; the kernel will do, which is what makes the framing agree with the
;; picture. It calls the dual's Scheme half, so there is still one curve
;; and no second definition to disagree with the first.
(define (curveplot-eval fn v params i x)
  (let ((d (wgsl-dual fn)))
    (if (not d)
        (error 'plot-posterior!
               (string-append (symbol->string fn)
                              ": not a dual — the plot needs a curve it can"
                              " evaluate here as well as on the device."
                              " Define it with define-dual")
               fn))
    (apply d
           (cons x (map (lambda (p)
                          (view-ref v (+ (shared-offset (curveplot-region p)) i)))
                        params)))))
