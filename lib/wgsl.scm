;;----------------------------------------------------------------------
;; A typed expression compiler from Scheme to WGSL
;;
;; The polestars are Houdini's VEX and Shadertoy, and what they share is a
;; PURE PER-ELEMENT KERNEL: Shadertoy's is pixel -> color, a wrangle's is
;; point -> attributes. Same kernel, different harness. This file is the
;; kernel half, so it is deliberately ignorant of pixels, points, buffers
;; and passes — it turns a Scheme expression into a WGSL expression and a
;; type, nothing more.
;;
;; It is a TYPE CHECKER rather than string templating, and that is the
;; entire reason it exists. WGSL does not coerce: `1` is i32, `1.0` is
;; f32, `1 + 1.0` is a compile error, and vec3<f32> * vec2<f32> is a
;; compile error. Emitting text without tracking types produces shaders
;; that fail in the browser, where the diagnostic is a shader compilation
;; log — the worst place in this system to debug anything. Carrying a type
;; on every expression moves those failures to a Scheme error at compile
;; time, where they can be tested headlessly.
;;
;; Integers are absent on purpose. Every numeric literal is emitted as f32
;; with a decimal point, which makes the i32/f32 literal hazard
;; unrepresentable rather than merely unlikely.
;;
;; Long type spellings (vec3<f32>, not vec3f) to match web/gpu.js: the
;; short aliases are newer and not universally available.
;;
;; Scalar-against-vector broadcast (`v * 2.0` giving a vector with every
;; component scaled) is assumed by the type rules here. It is what the
;; spec says and it is confirmed in practice; noted only because nothing
;; in this repo compiles WGSL, so that one rule rests on the language
;; rather than on a test.
;;----------------------------------------------------------------------

(define wgsl-types '(f32 vec2f vec3f vec4f))

(define (wgsl-type-name t)
  (cond ((eq? t 'f32)   "f32")
        ((eq? t 'vec2f) "vec2<f32>")
        ((eq? t 'vec3f) "vec3<f32>")
        ((eq? t 'vec4f) "vec4<f32>")
        (else (error 'wgsl "unknown type:" t))))

(define (wgsl-vec-width t)
  (cond ((eq? t 'f32) 1) ((eq? t 'vec2f) 2)
        ((eq? t 'vec3f) 3) ((eq? t 'vec4f) 4)
        (else 0)))

(define (wgsl-width->type n)
  (cond ((= n 1) 'f32) ((= n 2) 'vec2f)
        ((= n 3) 'vec3f) ((= n 4) 'vec4f)
        (else (error 'wgsl "no vector type of width" n))))

;; A compiled expression: its type, the statements that must precede it,
;; and the expression text itself. `let` is the only thing that produces
;; statements — WGSL has no block expression, so a binding has to be
;; hoisted out and named.
(define (wgsl-result type stmts code) (list type stmts code))
(define (wgsl-type-of r)  (car r))
(define (wgsl-stmts-of r) (cadr r))
(define (wgsl-code-of r)  (caddr r))

;; Locals are numbered so that flattening nested scopes into one WGSL
;; function body cannot collide, and so shadowing keeps working.
(define wgsl-counter 0)
(define (wgsl-fresh base)
  (set! wgsl-counter (+ wgsl-counter 1))
  (string-append base "_" (number->string wgsl-counter)))

;; Always emits a decimal point: (number->string 1) is "1", which WGSL
;; reads as i32.
(define (wgsl-number n) (number->string (* 1.0 n)))

;; Returns (type . emitted-name). A caller's environment is written the
;; obvious way, ((uv . vec2f) (time . f32)), where the WGSL name matches
;; the Scheme name. A `let` binds (name type . fresh-name) instead,
;; because its WGSL name is numbered — and getting that wrong is silent:
;; the statement declares d_1 while the body still says d, which is either
;; an unbound-name error in the shader or, worse, a reference to some
;; other d that happens to exist.
(define (wgsl-lookup name env)
  (let ((hit (assq name env)))
    (if (not hit) (error 'wgsl "unbound variable in kernel:" name))
    (let ((v (cdr hit)))
      (if (pair? v) v (cons v (symbol->string name))))))

;;--- the operator tables ------------------------------------------------

;; Component-wise, argument and result the same type.
(define wgsl-unary-same
  '((sin . "sin") (cos . "cos") (tan . "tan") (asin . "asin") (acos . "acos")
    (exp . "exp") (log . "log") (sqrt . "sqrt") (abs . "abs")
    (floor . "floor") (ceil . "ceil") (fract . "fract") (sign . "sign")
    (normalize . "normalize")))

;; Two arguments of the same type, result that type.
(define wgsl-binary-same
  '((min . "min") (max . "max") (pow . "pow") (atan2 . "atan2") (step . "step")))

;; Vector in, scalar out.
(define wgsl-vector-to-scalar '((length . "length")))

(define (wgsl-arith? op) (memq op '(+ - * /)))
(define (wgsl-arith-name op)
  (cond ((eq? op '+) "+") ((eq? op '-) "-")
        ((eq? op '*) "*") (else "/")))

;; The broadcast rule: identical types combine to themselves, and a scalar
;; combines with any vector to give that vector.
(define (wgsl-arith-type op ta tb)
  (cond ((eq? ta tb) ta)
        ((eq? ta 'f32) tb)
        ((eq? tb 'f32) ta)
        (else (error 'wgsl
                     (string-append "type mismatch in (" (symbol->string op) "): "
                                    (wgsl-type-name ta) " and " (wgsl-type-name tb))))))

(define (wgsl-check-same op ta tb)
  (if (not (eq? ta tb))
      (error 'wgsl
             (string-append "(" (symbol->string op) ") needs matching types, got: "
                            (wgsl-type-name ta) " and " (wgsl-type-name tb))))
  ta)

;;--- swizzles -----------------------------------------------------------

(define (wgsl-swizzle-ok? chars width)
  (let loop ((cs chars))
    (cond ((null? cs) #t)
          ((not (memv (car cs) '(#\x #\y #\z #\w))) #f)
          ((> (+ 1 (wgsl-component-index (car cs))) width) #f)
          (else (loop (cdr cs))))))

(define (wgsl-component-index c)
  (cond ((char=? c #\x) 0) ((char=? c #\y) 1)
        ((char=? c #\z) 2) (else 3)))

;;--- the compiler -------------------------------------------------------

(define (wgsl expr env)
  (cond
    ((number? expr) (wgsl-result 'f32 '() (wgsl-number expr)))
    ((symbol? expr)
     (let ((tn (wgsl-lookup expr env)))
       (wgsl-result (car tn) '() (cdr tn))))
    ((pair? expr)   (wgsl-form (car expr) (cdr expr) env))
    (else (error 'wgsl "cannot compile:" expr))))

(define (wgsl-form op args env)
  (cond
    ;; (vec2 a b) / (vec3 a b c) / (vec4 ...) — all components f32.
    ((memq op '(vec2 vec3 vec4))
     (let* ((want (cond ((eq? op 'vec2) 2) ((eq? op 'vec3) 3) (else 4)))
            (rs (map (lambda (a) (wgsl a env)) args)))
       (if (not (= (length rs) want))
           (error 'wgsl (string-append (symbol->string op) " needs "
                                       (number->string want) " components, got")
                  (length rs)))
       (for-each (lambda (r)
                   (if (not (eq? (wgsl-type-of r) 'f32))
                       (error 'wgsl
                              (string-append (symbol->string op)
                                             " components must be f32, got:")
                              (wgsl-type-name (wgsl-type-of r)))))
                 rs)
       (wgsl-result (wgsl-width->type want)
                    (wgsl-append-stmts rs)
                    (string-append (wgsl-type-name (wgsl-width->type want)) "("
                                   (wgsl-join (map wgsl-code-of rs) ", ") ")"))))

    ;; (let ((n e) ...) body) — hoisted into WGSL `let` statements, which
    ;; are immutable bindings, so the correspondence is exact.
    ((eq? op 'let)
     (let ((bindings (car args)) (body (cadr args)))
       (let loop ((bs bindings) (env env) (stmts '()))
         (if (null? bs)
             (let ((r (wgsl body env)))
               (wgsl-result (wgsl-type-of r)
                            (append stmts (wgsl-stmts-of r))
                            (wgsl-code-of r)))
             (let* ((name (caar bs))
                    (r (wgsl (cadr (car bs)) env))
                    (fresh (wgsl-fresh (symbol->string name))))
               (loop (cdr bs)
                     (cons (cons name (cons (wgsl-type-of r) fresh)) env)
                     (append stmts (wgsl-stmts-of r)
                             (list (string-append "let " fresh " : "
                                                  (wgsl-type-name (wgsl-type-of r))
                                                  " = " (wgsl-code-of r) ";")))))))))

    ;; (swizzle v xyz)
    ((eq? op 'swizzle)
     (let* ((r (wgsl (car args) env))
            (sw (cadr args))
            (chars (string->list (symbol->string sw)))
            (width (wgsl-vec-width (wgsl-type-of r))))
       (if (not (wgsl-swizzle-ok? chars width))
           (error 'wgsl (string-append "bad swizzle ." (symbol->string sw) " on "
                                       (wgsl-type-name (wgsl-type-of r)))))
       (wgsl-result (wgsl-width->type (length chars))
                    (wgsl-stmts-of r)
                    (string-append (wgsl-code-of r) "." (symbol->string sw)))))

    ;; (dot a b) — matching vectors in, scalar out.
    ((eq? op 'dot)
     (let ((a (wgsl (car args) env)) (b (wgsl (cadr args) env)))
       (wgsl-check-same 'dot (wgsl-type-of a) (wgsl-type-of b))
       (wgsl-result 'f32 (wgsl-append-stmts (list a b))
                    (string-append "dot(" (wgsl-code-of a) ", " (wgsl-code-of b) ")"))))

    ;; (mix a b t) / (clamp x lo hi) / (smoothstep e0 e1 x)
    ((memq op '(mix clamp smoothstep))
     (let* ((rs (map (lambda (a) (wgsl a env)) args))
            (t0 (wgsl-type-of (car rs))))
       (if (not (= (length rs) 3))
           (error 'wgsl (string-append (symbol->string op) " takes 3 arguments, got")
                  (length rs)))
       ;; mix's third argument may be a scalar blend factor; the others
       ;; must match the first.
       (wgsl-check-same op t0 (wgsl-type-of (cadr rs)))
       (let ((t2 (wgsl-type-of (caddr rs))))
         (if (and (not (eq? t2 t0)) (not (eq? t2 'f32)))
             (error 'wgsl (string-append "(" (symbol->string op)
                                         ") third argument must be "
                                         (wgsl-type-name t0) " or f32, got:")
                    (wgsl-type-name t2))))
       (wgsl-result t0 (wgsl-append-stmts rs)
                    (string-append (symbol->string op) "("
                                   (wgsl-join (map wgsl-code-of rs) ", ") ")"))))

    ((assq op wgsl-vector-to-scalar)
     (let ((r (wgsl (car args) env)))
       (wgsl-result 'f32 (wgsl-stmts-of r)
                    (string-append (cdr (assq op wgsl-vector-to-scalar))
                                   "(" (wgsl-code-of r) ")"))))

    ((assq op wgsl-unary-same)
     (let ((r (wgsl (car args) env)))
       (wgsl-result (wgsl-type-of r) (wgsl-stmts-of r)
                    (string-append (cdr (assq op wgsl-unary-same))
                                   "(" (wgsl-code-of r) ")"))))

    ((assq op wgsl-binary-same)
     (let ((a (wgsl (car args) env)) (b (wgsl (cadr args) env)))
       (wgsl-result (wgsl-check-same op (wgsl-type-of a) (wgsl-type-of b))
                    (wgsl-append-stmts (list a b))
                    (string-append (cdr (assq op wgsl-binary-same)) "("
                                   (wgsl-code-of a) ", " (wgsl-code-of b) ")"))))

    ;; Arithmetic, folded left so (+ a b c) is ((a + b) + c).
    ((wgsl-arith? op)
     (if (null? (cdr args))
         ;; Unary minus.
         (if (eq? op '-)
             (let ((r (wgsl (car args) env)))
               (wgsl-result (wgsl-type-of r) (wgsl-stmts-of r)
                            (string-append "-(" (wgsl-code-of r) ")")))
             (wgsl (car args) env))
         (let loop ((acc (wgsl (car args) env)) (rest (cdr args)))
           (if (null? rest)
               acc
               (let* ((b (wgsl (car rest) env))
                      (t (wgsl-arith-type op (wgsl-type-of acc) (wgsl-type-of b))))
                 (loop (wgsl-result t
                                    (append (wgsl-stmts-of acc) (wgsl-stmts-of b))
                                    (string-append "(" (wgsl-code-of acc) " "
                                                   (wgsl-arith-name op) " "
                                                   (wgsl-code-of b) ")"))
                       (cdr rest)))))))

    (else (error 'wgsl "unknown operator in kernel:" op))))

(define (wgsl-append-stmts rs)
  (if (null? rs) '() (append (wgsl-stmts-of (car rs)) (wgsl-append-stmts (cdr rs)))))

(define (wgsl-join strs sep)
  (cond ((null? strs) "")
        ((null? (cdr strs)) (car strs))
        (else (string-append (car strs) sep (wgsl-join (cdr strs) sep)))))

;;--- entry points -------------------------------------------------------

;; Compile a self-contained expression. Resets the local counter so the
;; emitted text depends only on the expression, which is what makes it
;; testable by string comparison.
(define (wgsl-compile expr env)
  (set! wgsl-counter 0)
  (wgsl expr env))

;; Just the expression text — errors if the expression needed statements,
;; since that text alone would not be valid on its own.
(define (wgsl-code expr env)
  (let ((r (wgsl-compile expr env)))
    (if (not (null? (wgsl-stmts-of r)))
        (error 'wgsl "expression needs statements; use wgsl-body"))
    (wgsl-code-of r)))

(define (wgsl-type expr env) (wgsl-type-of (wgsl-compile expr env)))

;; Statements plus a `return`, ready to drop into a WGSL function body.
(define (wgsl-body expr env indent)
  (let* ((r (wgsl-compile expr env))
         (lines (append (wgsl-stmts-of r)
                        (list (string-append "return " (wgsl-code-of r) ";")))))
    (wgsl-join (map (lambda (l) (string-append indent l)) lines) "\n")))
