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

;; :u32 exists for one reason: fold-i's index, and the declared
;; functions that take it to reach into a buffer. WGSL has no implicit
;; coercion, so it cannot quietly become an f32 — use (f32 k).
(define wgsl-types '(:f32 :u32 :bool :vec2f :vec3f :vec4f))

(define (wgsl-type-name t)
  (cond ((eq? t :f32)   "f32")
        ((eq? t :u32)   "u32")
        ((eq? t :bool)  "bool")
        ((eq? t :vec2f) "vec2<f32>")
        ((eq? t :vec3f) "vec3<f32>")
        ((eq? t :vec4f) "vec4<f32>")
        (else (error 'wgsl "unknown type:" t))))

(define (wgsl-vec-width t)
  (cond ((eq? t :f32) 1) ((eq? t :vec2f) 2)
        ((eq? t :vec3f) 3) ((eq? t :vec4f) 4)
        (else 0)))

(define (wgsl-width->type n)
  (cond ((= n 1) :f32) ((= n 2) :vec2f)
        ((= n 3) :vec3f) ((= n 4) :vec4f)
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
;; obvious way, ((uv . :vec2f) (time . :f32)), where the WGSL name matches
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
  (if (or (eq? ta :bool) (eq? tb :bool))
      (error 'wgsl (string-append "(" (symbol->string op)
                                  ") does not apply to bool")))
  (cond ((eq? ta tb) ta)
        ((eq? ta :f32) tb)
        ((eq? tb :f32) ta)
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

;; The kernel language deliberately accepts only orthodox ((name expr) ...)
;; bindings, a strict subset of what Scheme's own `let` here takes — vxs
;; also allows the flat vector form (let [a 1 b 2] ...), which is NOT
;; supported in a kernel and is not going to be.
;;
;; That restriction is fine; the diagnostic for it was not. Falling through
;; to (car (car bindings)) produced
;;
;;   car: contract violation, expected pair, got [c (- uv 0.5) r ...]
;;
;; which names none of: the form at fault, the language it is in, or what
;; the language wanted instead. A compiler whose entire justification is
;; turning GPU-time failures into legible compile-time errors does not get
;; to emit that.
(define (wgsl-check-bindings bs)
  (cond
    ((vector? bs)
     (error 'wgsl
            (string-append
             "let bindings must be a list of (name expr) pairs. "
             "The flat vector form [name expr ...] works in Scheme here, "
             "but a kernel takes only the orthodox spelling.")))
    ((not (list? bs))
     (error 'wgsl "let bindings must be a list of (name expr) pairs, got:" bs))
    (else
     (for-each
      (lambda (b)
        (if (or (not (pair? b))
                (not (symbol? (car b)))
                (not (pair? (cdr b)))
                (not (null? (cddr b))))
            (error 'wgsl "each let binding must be (name expr), got:" b)))
      bs))))

;;--- the compiler -------------------------------------------------------

(define (wgsl expr env)
  (cond
    ((number? expr) (wgsl-result :f32 '() (wgsl-number expr)))
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
                   (if (not (eq? (wgsl-type-of r) :f32))
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
    ;; `let` here binds SEQUENTIALLY — each binding is in scope for the
    ;; next — because it lowers to a run of WGSL `let` statements and there
    ;; is nothing to gain by pretending otherwise. So `let*` is the same
    ;; form, accepted because that is what a Scheme programmer writes when
    ;; they mean it, and the two must not appear to differ.
    ((or (eq? op 'let) (eq? op 'let*))
     (if (not (= (length args) 2))
         (error 'wgsl
                (string-append
                 "let takes a binding list and exactly ONE body expression; "
                 "a kernel has no sequencing, so extra body forms would be "
                 "silently discarded. Forms given:")
                (length args)))
     (wgsl-check-bindings (car args))
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

    ;; (f32 k) — the one conversion. WGSL has no implicit coercion, so a
    ;; fold index used as a quantity rather than an address has to say so.
    ((eq? op 'f32)
     (let ((r (wgsl (car args) env)))
       (if (not (memq (wgsl-type-of r) '(:u32 :f32)))
           (error 'wgsl (string-append "(f32) expects u32 or f32, got "
                                       (wgsl-type-name (wgsl-type-of r)))))
       (if (eq? (wgsl-type-of r) :f32)
           r
           (wgsl-result :f32 (wgsl-stmts-of r)
                        (string-append "f32(" (wgsl-code-of r) ")")))))

    ;; (fold-i N init (idx acc) body) — a bounded fold.
    ;;
    ;; A FOLD, NOT A LOOP: an accumulator and a compile-time bound, no
    ;; mutation, no break, no early exit. That is what keeps the language
    ;; pure-expression, which is the property everything else here rests
    ;; on — the whole form is still one value, so it nests inside
    ;; arithmetic and inside itself.
    ;;
    ;; The `var` in the emitted WGSL is an implementation detail of the
    ;; accumulator; nothing user-visible mutates. The bound must be a
    ;; literal because a GPU loop with a runtime bound is a different
    ;; performance object entirely, and because a static bound is what lets
    ;; the compiler keep its promise about how many random-* draws a kernel
    ;; consumes.
    ((eq? op 'fold-i)
     (if (not (= (length args) 4))
         (error 'wgsl "fold-i: expected (fold-i N init (idx acc) body)"))
     (let ((n (car args)) (init-x (cadr args)) (vars (caddr args)) (body (cadddr args)))
       (if (or (not (integer? n)) (< n 0))
           (error 'wgsl "fold-i: the bound must be a non-negative integer literal" n))
       (if (or (not (pair? vars)) (not (pair? (cdr vars)))
               (not (symbol? (car vars))) (not (symbol? (cadr vars)))
               (eq? (car vars) (cadr vars)))
           (error 'wgsl "fold-i: expected two distinct names (index accumulator)" vars))
       (let* ((idx (car vars))
              (accv (cadr vars))
              (r0 (wgsl init-x env))
              (atype (wgsl-type-of r0))
              (iname (wgsl-fresh (symbol->string idx)))
              (aname (wgsl-fresh (symbol->string accv)))
              (benv (cons (cons idx (cons :u32 iname))
                          (cons (cons accv (cons atype aname)) env)))
              (rb (wgsl body benv)))
         (if (not (eq? (wgsl-type-of rb) atype))
             (error 'wgsl
                    (string-append "fold-i: the body must have the accumulator's type, "
                                   "expected " (wgsl-type-name atype)
                                   " but got " (wgsl-type-name (wgsl-type-of rb)))))
         (wgsl-result
          atype
          (append
           (wgsl-stmts-of r0)
           (list (string-append "var " aname " : " (wgsl-type-name atype)
                                " = " (wgsl-code-of r0) ";")
                 (string-append "for (var " iname " : u32 = 0u; "
                                iname " < " (number->string n) "u; "
                                iname " = " iname " + 1u) {"))
           ;; The body's own let-lifts belong INSIDE the loop. Hoisting
           ;; them, as every other form here does, would evaluate them once
           ;; against the first index and reuse the answer for all of them.
           (map (lambda (l) (string-append "  " l)) (wgsl-stmts-of rb))
           (list (string-append "  " aname " = " (wgsl-code-of rb) ";")
                 "}"))
          aname))))

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
       (wgsl-result :f32 (wgsl-append-stmts (list a b))
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
         (if (and (not (eq? t2 t0)) (not (eq? t2 :f32)))
             (error 'wgsl (string-append "(" (symbol->string op)
                                         ") third argument must be "
                                         (wgsl-type-name t0) " or f32, got:")
                    (wgsl-type-name t2))))
       (wgsl-result t0 (wgsl-append-stmts rs)
                    (string-append (symbol->string op) "("
                                   (wgsl-join (map wgsl-code-of rs) ", ") ")"))))

    ((assq op wgsl-vector-to-scalar)
     (let ((r (wgsl (car args) env)))
       (wgsl-result :f32 (wgsl-stmts-of r)
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

    ;; Comparisons produce bool, which exists only to feed `if` and the
    ;; boolean connectives — there is no other way to make one and nothing
    ;; else consumes one.
    ((memq op '(< > <= >= =))
     (let ((a (wgsl (car args) env)) (b (wgsl (cadr args) env)))
       (if (not (and (eq? (wgsl-type-of a) :f32) (eq? (wgsl-type-of b) :f32)))
           (error 'wgsl
                  (string-append "(" (symbol->string op)
                                 ") compares scalars, got: "
                                 (wgsl-type-name (wgsl-type-of a)) " and "
                                 (wgsl-type-name (wgsl-type-of b)))))
       (wgsl-result :bool (wgsl-append-stmts (list a b))
                    (string-append "(" (wgsl-code-of a) " "
                                   (if (eq? op '=) "==" (symbol->string op))
                                   " " (wgsl-code-of b) ")"))))

    ((memq op '(and or))
     (let ((a (wgsl (car args) env)) (b (wgsl (cadr args) env)))
       (if (not (and (eq? (wgsl-type-of a) :bool) (eq? (wgsl-type-of b) :bool)))
           (error 'wgsl (string-append "(" (symbol->string op)
                                       ") needs bool operands")))
       (wgsl-result :bool (wgsl-append-stmts (list a b))
                    (string-append "(" (wgsl-code-of a)
                                   (if (eq? op 'and) " && " " || ")
                                   (wgsl-code-of b) ")"))))

    ((eq? op 'not)
     (let ((a (wgsl (car args) env)))
       (if (not (eq? (wgsl-type-of a) :bool))
           (error 'wgsl "(not) needs a bool operand"))
       (wgsl-result :bool (wgsl-stmts-of a)
                    (string-append "!(" (wgsl-code-of a) ")"))))

    ;; (if c a b) compiles to WGSL select(b, a, c) — note the reversed
    ;; argument order, which is select's own signature, not a mistake.
    ;;
    ;; This is NOT Scheme's `if`: select is branchless, so BOTH arms are
    ;; evaluated and neither is short-circuited. That is the right default
    ;; on a GPU, where a real branch diverges the warp, but it means an arm
    ;; must never be the thing guarding the other from a bad value — the
    ;; usual (if (> x 0) (/ 1 x) 0) idiom does not protect anything here.
    ((eq? op 'if)
     (let ((c (wgsl (car args) env))
           (a (wgsl (cadr args) env))
           (b (wgsl (caddr args) env)))
       (if (not (eq? (wgsl-type-of c) :bool))
           (error 'wgsl (string-append "(if) needs a bool condition, got: "
                                       (wgsl-type-name (wgsl-type-of c)))))
       (wgsl-check-same 'if (wgsl-type-of a) (wgsl-type-of b))
       (wgsl-result (wgsl-type-of a) (wgsl-append-stmts (list c a b))
                    (string-append "select(" (wgsl-code-of b) ", "
                                   (wgsl-code-of a) ", " (wgsl-code-of c) ")"))))

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

    ;; A declared or defined function. Last, so a built-in of the same name
    ;; always wins and no library can quietly redefine `sin`.
    ((wgsl-signature op)
     (let* ((sig (wgsl-signature op))
            (rs (map (lambda (a) (wgsl a env)) args)))
       (wgsl-check-args op sig rs)
       (wgsl-result (cadddr sig) (wgsl-append-stmts rs)
                    (string-append (cadr sig) "("
                                   (wgsl-join (map wgsl-code-of rs) ", ") ")"))))

    (else (error 'wgsl "unknown operator in kernel:" op))))

(define (wgsl-append-stmts rs)
  (if (null? rs) '() (append (wgsl-stmts-of (car rs)) (wgsl-append-stmts (cdr rs)))))

;; Join through a string output port, not a fold of string-append.
;;
;; The recursive version this replaces copied the entire accumulated tail
;; at every unwind step, which is O(n * total length) — measured at 200
;; lines it cost 0.18ms, at 8000 lines 152ms, quadrupling for every
;; doubling. A port appends into one growing buffer instead: 0.57ms at
;; 8000, and about twice as fast as (apply string-append ...) over an
;; interleaved list, which is linear too but builds 2n-1 intermediate
;; arguments first.
;;
;; This is cheap insurance rather than a fix for a present problem — real
;; kernels are a couple of hundred lines and compile once at setup, not per
;; frame. But every caller below joins something that grows with the
;; program being compiled, and quadratic is the wrong shape to leave in the
;; one function all of them go through.
(define (wgsl-join strs sep)
  (if (null? strs)
      ""
      (let ((p (open-output-string)))
        (display (car strs) p)
        (for-each (lambda (x) (display sep p) (display x p)) (cdr strs))
        (get-output-string p))))

;;--- callable functions -------------------------------------------------
;;
;; Two kinds, one table, and callers cannot tell them apart.
;;
;;   DECLARED  — a function hand-written in WGSL (lib/stat.wgsl and
;;               friends). Its signature is asserted here, because nothing
;;               can read it out of the WGSL text.
;;   DEFINED   — a function written in this language by define-gpu. Its
;;               argument types are declared, since nothing can infer
;;               those, but its RESULT type is derived by the same checker
;;               that checks everything else.
;;
;; The point of the second kind is that a mistake inside the function is
;; caught when the function is defined, and a mistake at a call site is
;; caught at the call. Neither reaches the WGSL compiler, which is the
;; whole reason this is a type checker and not a template. Inlining the
;; body at each call would have worked too, but it duplicates the code and
;; type-checks it once per call site instead of once.
;;
;; Because they share a table, a function can start as hand-written WGSL
;; and later be rewritten in Scheme without any caller changing.

;; (scheme-name wgsl-name (arg-type ...) result-type)
(define wgsl-signatures '())

(define (wgsl-declare! name wgsl-name arg-types result-type)
  (let loop ((xs wgsl-signatures) (acc '()) (found #f))
    (cond ((null? xs)
           (set! wgsl-signatures
                 (reverse (if found acc
                              (cons (list name wgsl-name arg-types result-type)
                                    acc)))))
          ((eq? (car (car xs)) name)
           ;; Replace rather than shadow: watch mode re-runs a file on every
           ;; save, and a table that only ever grows would accumulate a
           ;; stale entry per save.
           (loop (cdr xs) (cons (list name wgsl-name arg-types result-type) acc) #t))
          (else (loop (cdr xs) (cons (car xs) acc) found)))))

(define (wgsl-signature name) (assq name wgsl-signatures))

;; Scheme spells names with hyphens, WGSL with underscores.
(define (wgsl-fn-name name)
  (list->string (map (lambda (c) (if (char=? c #\-) #\_ c))
                     (string->list (symbol->string name)))))

(define (wgsl-check-args op sig rs)
  (let ((want (caddr sig)))
    (if (not (= (length rs) (length want)))
        (error 'wgsl
               (string-append "(" (symbol->string op) ") takes "
                              (number->string (length want))
                              " arguments, got")
               (length rs)))
    (let check ((got rs) (w want) (i 1))
      (if (not (null? got))
          (begin
            (if (not (eq? (wgsl-type-of (car got)) (car w)))
                (error 'wgsl
                       (string-append "(" (symbol->string op) ") argument "
                                      (number->string i) " must be "
                                      (wgsl-type-name (car w)) ", got: "
                                      (wgsl-type-name (wgsl-type-of (car got))))))
            (check (cdr got) (cdr w) (+ i 1)))))))

;;--- function definitions emitted into the module ------------------------
;; Kept in DEFINITION ORDER, and a redefinition replaces in place rather
;; than appending — both because watch mode re-runs a file on every save,
;; and because a function must appear before the code that calls it.

(define wgsl-definitions '())    ; ((name . source) ...) in emission order

(define (wgsl-put-definition! name source)
  (let loop ((xs wgsl-definitions) (acc '()) (found #f))
    (cond ((null? xs)
           (set! wgsl-definitions
                 (reverse (if found acc (cons (cons name source) acc)))))
          ((eq? (car (car xs)) name)
           (loop (cdr xs) (cons (cons name source) acc) #t))
          (else (loop (cdr xs) (cons (car xs) acc) found)))))

(define (wgsl-definitions-source)
  (wgsl-join (map cdr wgsl-definitions) "\n"))

(define (wgsl-forget-definitions!)
  (set! wgsl-definitions '()))

;; (wgsl-define-fn! name ((arg type) ...) body) — compile, derive the
;; result type, emit a WGSL fn, and register the signature.
(define (wgsl-define-fn! name params body)
  (for-each
   (lambda (p)
     (if (or (not (pair? p)) (not (symbol? (car p)))
             (not (pair? (cdr p))) (not (memq (cadr p) wgsl-types)))
         (error 'wgsl
                (string-append "define-gpu: each parameter must be "
                               "(name type), got ")
                p)))
   params)
  (let* ((env (map (lambda (p) (cons (car p) (cadr p))) params))
         (r (wgsl-compile body env))
         (ret (wgsl-type-of r))
         (wname (wgsl-fn-name name))
         (lines (append (wgsl-stmts-of r)
                        (list (string-append "return " (wgsl-code-of r) ";")))))
    (wgsl-put-definition!
     name
     (string-append
      "fn " wname "("
      (wgsl-join (map (lambda (p)
                        (string-append (symbol->string (car p)) " : "
                                       (wgsl-type-name (cadr p))))
                      params)
                 ", ")
      ") -> " (wgsl-type-name ret) " {\n"
      (wgsl-join (map (lambda (l) (string-append "  " l)) lines) "\n")
      "\n}\n"))
    (wgsl-declare! name wname (map cadr params) ret)
    ret))

;; (define-gpu (name (arg type) ...) body)
;;
;; A macro for the same reason define-kernel is one: the body is code in
;; another language and must not be evaluated as Scheme. The result type is
;; deliberately NOT declared — deriving it is what makes the signature
;; honest rather than an assertion that could drift from the body.
(defmacro (define-gpu spec body)
  `(wgsl-define-fn! ',(car spec) ',(cdr spec) ',body))

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
