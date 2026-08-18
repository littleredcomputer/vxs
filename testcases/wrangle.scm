;;; wrangle.scm: VEX-style attribute wrangle over a point cube
;;;
;;; Starter for the shader-DSL / GPU-particle-inference direction. Two
;;; algebras live here, and the split is the whole point:
;;;
;;;   NODE   points -> points   touches memory; a future compute dispatch
;;;   FIELD  pos    -> value    pure; a future WGSL *function*
;;;
;;; Fields compose with each other (warp, fbm, curl, ...) and flatten to one
;;; function. Exactly one bridge -- `advect` -- turns a field into a node.
;;; Nothing here touches the GPU yet; this exists to find out whether the
;;; composition reads well before any of it owes anything to WGSL.

;; --- vec3 helpers ---

(define (v3 x y z) (vector x y z))
(define (v3x p) (vector-ref p 0))
(define (v3y p) (vector-ref p 1))
(define (v3z p) (vector-ref p 2))

(define (v3+ a b) (v3 (+ (v3x a) (v3x b)) (+ (v3y a) (v3y b)) (+ (v3z a) (v3z b))))
(define (v3- a b) (v3 (- (v3x a) (v3x b)) (- (v3y a) (v3y b)) (- (v3z a) (v3z b))))
(define (v3scale s a) (v3 (* s (v3x a)) (* s (v3y a)) (* s (v3z a))))

;; Generic over scalar-or-vector so the field combinators below don't have to
;; care which kind of field they were handed.
(define (val+ a b) (if (vector? a) (v3+ a b) (+ a b)))
(define (val* s a) (if (vector? a) (v3scale s a) (* s a)))

;;; ============================ FIELDS ============================
;;; A field is an ordinary unary procedure, pos -> value. Scalar fields
;;; return numbers, vector fields return v3.
;;;
;;; What a field CLOSES OVER (time, frequency, phase) is what becomes a GPU
;;; *uniform* later. What a field's STRUCTURE is -- which combinators were
;;; applied to what -- is what becomes the shader. Keeping those separate is
;;; why rebuilding a field every frame below costs a closure, not a recompile.

(define (fconst k) (lambda (p) k))
(define fpos (lambda (p) p))            ; identity / position field

;; Base scalar noise: cheap deterministic stand-in for perlin/simplex, same
;; shape (pos -> number) so a real one drops straight in.
(define (wave t)
  (lambda (p)
    (* 0.5 (+ (sin (+ (v3x p) t))
              (cos (+ (* 1.3 (v3y p)) (* 0.9 t)))
              (sin (+ (* 0.7 (v3z p)) (* 1.1 t)))))))

;; --- combinators ---

(define (f+ . fs)
  (lambda (p)
    (let loop ((rest (cdr fs)) (acc ((car fs) p)))
      (if (null? rest)
          acc
          (loop (cdr rest) (val+ acc ((car rest) p)))))))

;; Scaling the OUTPUT and scaling the DOMAIN are different operations and
;; both are needed (fbm uses one of each per octave).
(define (fscale s f) (lambda (p) (val* s (f p))))       ; output
(define (fzoom  s f) (lambda (p) (f (v3scale s p))))    ; domain
(define (fshift d f) (lambda (p) (f (v3+ p d))))        ; domain

;; Domain distortion: sample f wherever vector field g pushes us to.
;; Scale g first (via fscale) to control how hard it pushes.
(define (warp f g) (lambda (p) (f (v3+ p (g p)))))

;; scalar field -> vector field, by central differences.
(define grad-eps 0.01)
(define (grad f)
  (lambda (p)
    (let ((ex (v3 grad-eps 0.0 0.0))
          (ey (v3 0.0 grad-eps 0.0))
          (ez (v3 0.0 0.0 grad-eps))
          (k (/ 1.0 (* 2.0 grad-eps))))
      (v3 (* k (- (f (v3+ p ex)) (f (v3- p ex))))
          (* k (- (f (v3+ p ey)) (f (v3- p ey))))
          (* k (- (f (v3+ p ez)) (f (v3- p ez))))))))

;; Three scalar potentials -> a divergence-free vector field. Divergence-free
;; is what keeps advected points from collapsing into sinks, so this is the
;; field you actually want driving particles.
(define (curl3 p1 p2 p3)
  (let ((g1 (grad p1)) (g2 (grad p2)) (g3 (grad p3)))
    (lambda (p)
      (let ((a (g1 p)) (b (g2 p)) (c (g3 p)))
        (v3 (- (v3y c) (v3z b))
            (- (v3z a) (v3x c))
            (- (v3x b) (v3y a)))))))

;; Octave sum. Scalar fields only.
(define (fbm octaves base)
  (lambda (p)
    (let loop ((i 0) (freq 1.0) (amp 1.0) (acc 0.0) (norm 0.0))
      (if (= i octaves)
          (/ acc norm)
          (loop (+ i 1) (* freq 2.0) (* amp 0.5)
                (+ acc (* amp (base (v3scale freq p))))
                (+ norm amp))))))

;;; ============================ NODES ============================
;;; A node is points -> points. @P/@ptnum are deliberately unhygienic ambient
;;; bindings point-wrangle injects into the caller's body -- that's the point
;;; of the sigil, and it's an ordinary macro rather than VEX's parser hack.

(defmacro (point-wrangle points . body)
  `(let ((n# (vector-length ,points)))
     (do ((i# 0 (+ i# 1)))
         ((= i# n#))
       (let ((@P (vector-ref ,points i#))
             (@ptnum i#))
         (vector-set! ,points i# (begin ,@body))))))

(define (as-node wrangle-thunk)
  (lambda (points) (wrangle-thunk points) points))

(define (pipe . nodes)
  (lambda (points)
    (for-each (lambda (n) (n points)) nodes)
    points))

;; A node is a procedure and so is a field, so `switch` needs no idea which
;; it's routing -- it works at either level. (See both uses below.)
(define (switch selector . branches)
  (lambda (x) ((list-ref branches (selector)) x)))

;; --- THE bridge: field -> node. The only place the two algebras meet. ---
(define (advect f dt)
  (as-node (lambda (pts)
             (point-wrangle pts (v3+ @P (v3scale dt (f @P)))))))

;; --- cube generator ---

(define (make-point-cube n spacing)
  (let* ((total (* n n n))
         (points (make-vector total))
         (offset (* -0.5 spacing (- n 1))))
    (do ((i 0 (+ i 1)))
        ((= i total) points)
      (let ((xi (modulo i n))
            (yi (modulo (quotient i n) n))
            (zi (quotient i (* n n))))
        (vector-set! points i
          (v3 (+ offset (* spacing xi))
              (+ offset (* spacing yi))
              (+ offset (* spacing zi))))))))

;;; ============================ PROOF ============================

;; Composed flow fields. Note these are *constructors* taking t: the closure
;; captures it, so per-frame rebuilding costs an allocation here and would
;; cost a uniform write on the GPU -- never a shader recompile.
(define (flow t)
  (curl3 (fzoom 0.6 (wave t))
         (fzoom 0.6 (wave (+ t 17.0)))
         (fzoom 0.6 (wave (+ t 31.0)))))

;; A flow field warped by a slower copy of itself -- pure composition,
;; no new machinery.
(define (warped-flow t)
  (warp (flow t) (fscale 0.4 (flow (* 0.5 t)))))

;; Same base noise, four ways -- shows the combinators are actually
;; independent of each other and of what they're applied to.
(define probe (v3 0.3 -0.7 1.1))
(define base (wave 0.0))

(display "--- one scalar field, sampled at ") (write probe) (newline)
(display "  base            : ") (write (base probe)) (newline)
(display "  fzoom 2.0       : ") (write ((fzoom 2.0 base) probe)) (newline)
(display "  fscale 2.0      : ") (write ((fscale 2.0 base) probe)) (newline)
(display "  fbm 4 octaves   : ") (write ((fbm 4 base) probe)) (newline)
(display "  grad -> vector  : ") (write ((grad base) probe)) (newline)
(newline)

(display "--- vector fields at same point ---") (newline)
(display "  curl3 flow      : ") (write ((flow 0.0) probe)) (newline)
(display "  warped flow     : ") (write ((warped-flow 0.0) probe)) (newline)
(newline)

;; Divergence-free check: curl3's whole reason for existing. Numerically
;; estimate div(F) and confirm it's ~0, unlike a raw gradient field.
(define (divergence f)
  (lambda (p)
    (let ((ex (v3 grad-eps 0.0 0.0))
          (ey (v3 0.0 grad-eps 0.0))
          (ez (v3 0.0 0.0 grad-eps))
          (k (/ 1.0 (* 2.0 grad-eps))))
      (+ (* k (- (v3x (f (v3+ p ex))) (v3x (f (v3- p ex)))))
         (* k (- (v3y (f (v3+ p ey))) (v3y (f (v3- p ey)))))
         (* k (- (v3z (f (v3+ p ez))) (v3z (f (v3- p ez)))))))))

(display "--- divergence (curl3 should be ~0, gradient field should not) ---") (newline)
(display "  div(curl3 flow) : ") (write ((divergence (flow 0.0)) probe)) (newline)
(display "  div(grad base)  : ") (write ((divergence (grad base)) probe)) (newline)
(newline)

;; --- the functor: fields drive points ---

(define cube (make-point-cube 3 1.0))
(define t 0.0)
(define dt 0.05)

(display "--- advection: field -> node ---") (newline)
(display "  cube[0]  before : ") (write (vector-ref cube 0)) (newline)
(display "  cube[13] before : ") (write (vector-ref cube 13)) (newline)

;; `switch` routing FIELD constructors, not nodes -- same combinator, and the
;; advect below never learns which field it got.
(define subject 0)
(define pick-field (switch (lambda () subject) flow warped-flow))

(do ((step 0 (+ step 1)))
    ((= step 10))
  ((advect (pick-field t) dt) cube)     ; field rebuilt per step: uniforms, not recompiles
  (set! t (+ t dt)))

(display "  cube[0]  flow   : ") (write (vector-ref cube 0)) (newline)
(display "  cube[13] flow   : ") (write (vector-ref cube 13)) (newline)

;; Swap the subject. Everything downstream -- advect, the wrangle, the cube --
;; is untouched.
(set! subject 1)

(do ((step 0 (+ step 1)))
    ((= step 10))
  ((advect (pick-field t) dt) cube)
  (set! t (+ t dt)))

(display "  cube[0]  warped : ") (write (vector-ref cube 0)) (newline)
(display "  cube[13] warped : ") (write (vector-ref cube 13)) (newline)
(newline)

;; --- nodes still compose as nodes ---
(define shrink (as-node (lambda (pts) (point-wrangle pts (v3scale 0.99 @P)))))
(define settle (pipe (advect (flow t) dt) shrink))

(settle cube)
(display "--- pipe (advect + shrink) ---") (newline)
(display "  cube[0]  piped  : ") (write (vector-ref cube 0)) (newline)
(newline)

(display "=== wrangle.scm: field algebra + node algebra + one bridge ===\n")
