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
;; The matrix types are here for ONE purpose: to be a wide accumulator for
;; a bounded fold, so a scan can carry more state than a vector's four
;; components. They are a state BUNDLE whose WGSL spelling happens to be a
;; matrix — the same move :quat already makes, where the storage type
;; carries the intent and the type system carries the shape.
;;
;; Which is why there is no matrix multiply, and should not be one until
;; something actually wants linear algebra: exposing it would let the
;; checker accept `(* state1 state2)`, an operation with a type and no
;; meaning, in a file whose whole value is refusing exactly that.
;;
;; Columns are always FOUR components. WGSL allows matCxR for C and R in
;; 2..4, but a vec3 column pads to 16 bytes inside a storage array, and
;; picking one column height sidesteps that question permanently rather
;; than documenting it. It also makes the element arithmetic trivial:
;; element k lives at column k/4, row k%4.
(define wgsl-types
  '(:f32 :u32 :bool :vec2f :vec3f :vec4f :mat2x4f :mat3x4f :mat4x4f))

;; (columns . total components), in increasing capacity.
(define wgsl-mat-shapes
  '((:mat2x4f 2 8) (:mat3x4f 3 12) (:mat4x4f 4 16)))

(define (wgsl-mat-shape t) (assq t wgsl-mat-shapes))

;; The narrowest type that holds n scalars, vectors first. #f when nothing
;; does — sixteen is the ceiling and it is the device's, not a shortcut.
(define (wgsl-bundle-type n)
  (cond ((< n 1) #f)
        ((<= n 1) :f32)
        ((<= n 2) :vec2f)
        ((<= n 3) :vec3f)
        ((<= n 4) :vec4f)
        ((<= n 8) :mat2x4f)
        ((<= n 12) :mat3x4f)
        ((<= n 16) :mat4x4f)
        (else #f)))

(define (wgsl-bundle-width t)
  (cond ((eq? t :f32) 1) ((eq? t :vec2f) 2) ((eq? t :vec3f) 3) ((eq? t :vec4f) 4)
        ((wgsl-mat-shape t) (caddr (wgsl-mat-shape t)))
        (else 0)))

(define (wgsl-type-name t)
  (cond ((eq? t :f32)   "f32")
        ((eq? t :u32)   "u32")
        ;; Not a WGSL type. It names the result of a TERMINAL form — one
        ;; that writes rather than evaluates — so it exists only to be
        ;; rejected by every operator, which is what confines such a form
        ;; to the last position of a body.
        ((eq? t :point)  "a terminal form")
        ((eq? t :bool)  "bool")
        ((eq? t :vec2f) "vec2<f32>")
        ((eq? t :vec3f) "vec3<f32>")
        ((eq? t :vec4f) "vec4<f32>")
        ((eq? t :mat2x4f) "mat2x4<f32>")
        ((eq? t :mat3x4f) "mat3x4<f32>")
        ((eq? t :mat4x4f) "mat4x4<f32>")
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
;;
;; The name is also SANITISED, for the same reason a function's is: a
;; WGSL identifier admits letters, digits and underscore and nothing
;; else, while the ordinary Scheme spellings — `my-var`, `outside?`,
;; `set!` — use all three of the characters it forbids. Emitting them
;; produced text this type checker accepted happily and the browser's
;; shader compiler rejected, which is precisely the class of failure this
;; file exists to move to Scheme.
;;
;; The mapping is INJECTIVE, because `a?` and `a!` are different names
;; and must stay different. It used to turn every illegal character into
;; an underscore, which is what the comment here claimed was enough —
;; and it was not: `a?` and `a!` both became `a_`, and so did `a-`.
;; Locals survived only because wgsl-fresh numbers them; two functions
;; named `ok?` and `ok!` emitted two `fn ok_` and a shader that would not
;; compile. So the common Scheme characters get their own spellings, and
;; anything else escapes by code point:
;;
;;   -  -> _      ? -> _p     ! -> _b     * -> _s     > -> _g
;;   <  -> _l     = -> _e     / -> _d     % -> _c     _ -> __
;;   anything else -> _x<code>_
;;
;; `_` itself doubles, so an escape can never be mistaken for one:
;; `a_p` came from `a_p` (a__p) and `a?` gives a_p. The hyphen keeps its
;; lone underscore, since it is by far the commonest character here and
;; its WGSL spelling is the one a reader of emitted code expects.
(define wgsl-counter 0)
(define (wgsl-fresh base)
  (set! wgsl-counter (+ wgsl-counter 1))
  (string-append (wgsl-underscore base) "_" (number->string wgsl-counter)))

(define wgsl-ident-escapes
  '((#\- . "_") (#\? . "_p") (#\! . "_b") (#\* . "_s") (#\> . "_g")
    (#\< . "_l") (#\= . "_e") (#\/ . "_d") (#\% . "_c") (#\_ . "__")))

(define (wgsl-ident-piece c)
  (cond ((or (char-alphabetic? c) (char-numeric? c)) (string c))
        ((assv c wgsl-ident-escapes) => cdr)
        (else (string-append "_x" (number->string (char->integer c)) "_"))))

(define (wgsl-underscore s)
  (let ((p (open-output-string)))
    (for-each (lambda (c) (display (wgsl-ident-piece c) p)) (string->list s))
    (get-output-string p)))

;; Bind a result to a fresh local, so its code can be MENTIONED twice
;; without being EVALUATED twice. Compiled results are spliced as text, so
;; an operator whose expansion repeats an operand repeats whatever that
;; operand does — and the generator is stateful, so (modulo (random-normal
;; ...) k) written naively would draw two different numbers and combine
;; them. Anything that expands to more than one mention of an argument
;; must come through here.
(define (wgsl-bind base r)
  (let ((fresh (wgsl-fresh base)))
    (wgsl-result (wgsl-type-of r)
                 (append (wgsl-stmts-of r)
                         (list (string-append "let " fresh " : "
                                              (wgsl-type-name (wgsl-type-of r))
                                              " = " (wgsl-code-of r) ";")))
                 fresh)))

;; Always emits a decimal point: (number->string 1) is "1", which WGSL
;; reads as i32.
;; A NON-FINITE literal becomes a call, not a number. WGSL has no spelling
;; for an infinity or a NaN and will not let you compute one at
;; shader-creation time — `log(0.0)` and `-1.0 / 0.0` are both
;; const-expressions whose value cannot be represented, so both are
;; compile errors. The helpers below hide a runtime `var` to divide by,
;; which is the only way to reach the IEEE value.
;;
;; So a kernel writes `-inf` the way Scheme already spells it — the reader
;; reads it as a number, and this is where it stops being one. A model
;; wanting to say "this configuration is impossible" needs no vocabulary
;; it would not otherwise have.
;;
;; ⚠️ WGSL permits an implementation to assume infinities and NaNs do not
;; arise, and to yield an indeterminate value where one would. Hardware
;; f32 produces them, so this works in practice — but it is the language
;; declining to promise, not a guarantee, and it wants an eye on a real
;; device before a score depends on it.
(define (wgsl-number n)
  (cond ((finite? n) (number->string (* 1.0 n)))
        ((infinite? n) (if (< n 0) "neg_inf()" "pos_inf()"))
        (else "nan_f32()")))

;; Emitted as DEFINITIONS rather than written into lib/stat.wgsl, because
;; a shadertoy shader does not include that file and would otherwise call
;; a function nothing defines.
;;
;; The `var` is the whole trick in each and must not become a `let` or a
;; `const`: it makes the operand runtime storage, so the division is a
;; runtime operation and yields the IEEE value instead of being evaluated
;; by the compiler and rejected.
(define (wgsl-register-nonfinite-helpers!)
  (wgsl-put-definition!
   'neg-inf "fn neg_inf() -> f32 {\n  var z : f32 = 0.0;\n  return -1.0 / z;\n}\n")
  (wgsl-put-definition!
   'pos-inf "fn pos_inf() -> f32 {\n  var z : f32 = 0.0;\n  return 1.0 / z;\n}\n")
  (wgsl-put-definition!
   'nan-f32 "fn nan_f32() -> f32 {\n  var z : f32 = 0.0;\n  return z / z;\n}\n"))

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
      ;; Underscored, and it must match how the parameter was DECLARED in
      ;; wgsl-define-fn! — the two spellings are the same name and have to
      ;; travel together.
      (if (pair? v) v (cons v (wgsl-underscore (symbol->string name)))))))

;;--- the operator tables ------------------------------------------------

;; Component-wise, argument and result the same type.
(define wgsl-unary-same
  '((sin . "sin") (cos . "cos") (tan . "tan") (asin . "asin") (acos . "acos")
    (exp . "exp") (log . "log") (sqrt . "sqrt") (abs . "abs")
    (floor . "floor") (ceil . "ceil") (fract . "fract") (sign . "sign")
    (normalize . "normalize")
    ;; The screen-space derivatives. Fragment-only — see wgsl-stage-of.
    (dpdx . "dpdx") (dpdy . "dpdy") (fwidth . "fwidth")))

;;--- the one place a stage matters --------------------------------------
;; Almost every operation is legal in any shader, and this table is the
;; exception list rather than the beginning of a stage system.
;;
;; dpdx and its family are computed by differencing across the 2x2 quad a
;; FRAGMENT shader executes in. There is no quad in a compute shader, so
;; the browser's shader compiler rejects them there — which is the one
;; place a mistake must not be allowed to reach, since a shader log in a
;; console is what this whole file exists to avoid.
;;
;; What they are FOR: a uniform-width stroke needs the distance to a
;; curve, and normalising an implicit function by its screen-space
;; gradient gives that distance already in PIXELS —
;;
;;   (/ (abs g) (length (vec2 (dpdx g) (dpdy g))))
;;
;; for g the residual field. The analytic alternative, |g|/sqrt(1+f'^2),
;; gives plot units and then needs the slope rescaled by the axes' aspect
;; before it means anything on screen; dpdy picks the vertical scale up on
;; its own. So this is the better mechanism and not merely the cheaper
;; one, and it is why the near-SDF wants no derivative machinery at all.
(define wgsl-stage-of
  '((dpdx . :fragment) (dpdy . :fragment) (fwidth . :fragment)))

;; Which stage is being compiled, or #f for "not saying". A harness sets
;; it around its own compile; nothing else should need to.
(define-once wgsl-stage #f)

;; Refuse a stage-restricted operation when the stage does not match —
;; INCLUDING when nothing said what the stage is. "Not saying" must not
;; count as permission: an unstaged compile is precisely the case where
;; nothing knows where the code will end up, which is the case where the
;; browser gets to find out instead.
(define (wgsl-check-stage op)
  (let ((want (assq op wgsl-stage-of)))
    (if (and want (not (eq? (cdr want) wgsl-stage)))
        (error 'wgsl
               (string-append
                "(" (symbol->string op) ") is only available in a "
                (symbol->string (cdr want)) " shader"
                (if wgsl-stage
                    (string-append ", and this is a "
                                   (symbol->string wgsl-stage) " one")
                    ", and this compile did not say which stage it is"))))))

;;--- a literal that cannot survive shader creation ----------------------
;; WGSL evaluates a const-expression at shader-creation time, and a
;; literal argument makes one. `log(0.0)` is therefore not -inf: it is the
;; error "value -Infinity cannot be represented as '<AbstractFloat>'",
;; reported by the browser's shader compiler with no line number in our
;; source — which is the failure this whole file exists to catch instead.
;;
;; Learned the hard way. logpdf-exponential's dual body said (log 0.0),
;; which is correct Scheme, and put a shader that would not compile into
;; every assembled module. The host equivalents below are consulted ONLY
;; to refuse such a call; nothing here folds constants.
;;
;; For -inf, which is usually what was wanted: lib/stat.wgsl's neg_inf.
(define wgsl-const-check
  (list (cons 'log log) (cons 'sqrt sqrt) (cons 'exp exp)
        (cons 'asin asin) (cons 'acos acos)))

(define (wgsl-check-const op args)
  (let ((chk (assq op wgsl-const-check)))
    (if (and chk (pair? args) (null? (cdr args)) (number? (car args)))
        (let ((r ((cdr chk) (car args))))
          (if (not (finite? r))
              (error 'wgsl
                     (string-append
                      "(" (symbol->string op) " " (number->string (car args))
                      ") is " (if (infinite? r) "infinite" "not a number")
                      ", and WGSL evaluates a literal argument at"
                      " shader-creation time and rejects that."
                      " Write the value itself — a non-finite literal"
                      " such as -inf is emitted as a runtime helper.")))))))

;; Run a compile with the stage declared, and put it back afterwards even
;; if the compile raises — a harness that left the stage set would license
;; a later unstaged compile to use fragment-only operations.
(defmacro (with-wgsl-stage stage . body)
  `(let ((was# wgsl-stage))
     (set! wgsl-stage ,stage)
     (unwind-protect (begin ,@body) (set! wgsl-stage was#))))

;; Two arguments of the same type, result that type.
;;
;; `expt` is here as well as `pow` because a define-dual body has to be
;; valid in BOTH languages, and Scheme spells this one `expt` — `pow` is
;; not a Scheme procedure, so a dual written with it would type-check for
;; the device and then fail as an unbound variable on its first host call.
;; min and max are NOT here: Scheme's are n-ary, so they get a folding
;; branch of their own in wgsl-form.
(define wgsl-binary-same
  '((pow . "pow") (expt . "pow")
    (atan2 . "atan2") (step . "step")))

;; Vector in, scalar out.
(define wgsl-vector-to-scalar '((length . "length")))

(define (wgsl-arith? op) (memq op '(+ - * /)))
(define (wgsl-arith-name op)
  (cond ((eq? op '+) "+") ((eq? op '-) "-")
        ((eq? op '*) "*") (else "/")))

;; The broadcast rule: identical types combine to themselves, and a scalar
;; combines with any vector to give that vector.
;; Which types arithmetic applies to at all. The previous form rejected
;; :bool by name and then said "if either side is :f32, take the other" —
;; which let ANYTHING through when paired with a float, including a
;; terminal form's :point, and typed (u32 * 3.0) as u32 while emitting
;; WGSL that will not compile. Naming what is allowed rather than what is
;; not means a new type is excluded until someone decides otherwise.
(define (wgsl-vector-type? t) (memq t '(:vec2f :vec3f :vec4f)))
(define (wgsl-arith-type? t) (or (eq? t :f32) (eq? t :u32) (wgsl-vector-type? t)))

(define (wgsl-arith-type op ta tb)
  (if (not (and (wgsl-arith-type? ta) (wgsl-arith-type? tb)))
      (error 'wgsl (string-append "(" (symbol->string op) ") does not apply to "
                                  (wgsl-type-name (if (wgsl-arith-type? ta) tb ta)))))
  (cond ((eq? ta tb) ta)
        ;; A scalar broadcasts across a VECTOR, and only there. An f32 does
        ;; not silently combine with a u32 — WGSL will not, and the
        ;; conversion that would make it work is the one worth writing.
        ((and (eq? ta :f32) (wgsl-vector-type? tb)) tb)
        ((and (eq? tb :f32) (wgsl-vector-type? ta)) ta)
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

;;--- arity --------------------------------------------------------------
;; The table-driven forms each read a fixed number of arguments and used
;; to read them BLINDLY: (sin time time) dropped the second operand,
;; (and a b c) dropped its third conjunct, and (< 0.0 t 1.0) — the
;; idiomatic bounds check — compiled to (0.0 < t) and lost its upper
;; bound while looking exactly right. A count is either given Scheme's
;; own n-ary meaning below, or refused here with the form named.
(define (wgsl-arity op args n)
  (if (not (= (length args) n))
      (error 'wgsl
             (string-append "(" (symbol->string op) ") takes "
                            (number->string n)
                            (if (= n 1) " argument, got" " arguments, got"))
             (length args))))

(define (wgsl-arity-at-least op args n)
  (if (< (length args) n)
      (error 'wgsl
             (string-append "(" (symbol->string op) ") needs at least "
                            (number->string n) " arguments, got")
             (length args))))

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
  ;; Before anything else, and for every form: a stage-restricted
  ;; operation is refused where it cannot run, and so is a literal
  ;; argument whose value WGSL would compute at shader-creation time and
  ;; then refuse. Both checks are keyed by name, so both cover built-ins
  ;; and declared functions alike.
  (wgsl-check-stage op)
  (wgsl-check-const op args)
  (cond
    ;; (vec2 a b) / (vec3 a b c) / (vec4 ...) — all components f32.
    ;; (mat2x4 a b c ...) — all components, COLUMN-MAJOR, which is the
    ;; order WGSL's own component-wise matrix constructor takes. Columns
    ;; are not accepted as arguments: a caller assembling a state bundle
    ;; has scalars, and one way in is one way to get it wrong.
    ((memq op '(mat2x4 mat3x4 mat4x4))
     (let* ((t (cond ((eq? op 'mat2x4) :mat2x4f)
                     ((eq? op 'mat3x4) :mat3x4f)
                     (else :mat4x4f)))
            (want (caddr (wgsl-mat-shape t)))
            (rs (map (lambda (a) (wgsl a env)) args)))
       (if (not (= (length rs) want))
           (error 'wgsl (string-append (symbol->string op) " needs "
                                       (number->string want)
                                       " components, column-major, got")
                  (length rs)))
       (for-each (lambda (r)
                   (if (not (eq? (wgsl-type-of r) :f32))
                       (error 'wgsl
                              (string-append (symbol->string op)
                                             " components must be f32, got:")
                              (wgsl-type-name (wgsl-type-of r)))))
                 rs)
       (wgsl-result t (wgsl-append-stmts rs)
                    (string-append (wgsl-type-name t) "("
                                   (wgsl-join (map wgsl-code-of rs) ", ") ")"))))

    ;; (mat-col m i) — the i-th column, as a vec4f. The index is a literal
    ;; because WGSL wants a constant there and because a computed one would
    ;; be a gather into a register file, which is not a thing.
    ((eq? op 'mat-col)
     (if (not (= (length args) 2))
         (error 'wgsl "mat-col: expected (mat-col m i)"))
     (let* ((m (wgsl (car args) env))
            (i (cadr args))
            (shape (wgsl-mat-shape (wgsl-type-of m))))
       (if (not shape)
           (error 'wgsl "mat-col: not a matrix, got:"
                  (wgsl-type-name (wgsl-type-of m))))
       (if (or (not (integer? i)) (< i 0) (>= i (cadr shape)))
           (error 'wgsl
                  (string-append "mat-col: column must be a literal 0.."
                                 (number->string (- (cadr shape) 1)) ", got")
                  i))
       (wgsl-result :vec4f (wgsl-stmts-of m)
                    (string-append (wgsl-code-of m) "[" (number->string i) "]"))))

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
     (wgsl-arity op args 1)
     (let ((r (wgsl (car args) env)))
       (if (not (memq (wgsl-type-of r) '(:u32 :f32)))
           (error 'wgsl (string-append "(f32) expects u32 or f32, got "
                                       (wgsl-type-name (wgsl-type-of r)))))
       (if (eq? (wgsl-type-of r) :f32)
           r
           (wgsl-result :f32 (wgsl-stmts-of r)
                        (string-append "f32(" (wgsl-code-of r) ")")))))

    ;; (u32 n) — an integer literal, or a conversion from f32.
    ;;
    ;; Needed because every bare number in this language emits as f32:
    ;; (* a 3) gives "a * 3.0", which will not typecheck against a u32.
    ;; Explicit rather than inferred, on the same principle as (f32 k) —
    ;; a conversion is worth seeing.
    ((eq? op 'u32)
     (wgsl-arity op args 1)
     (let ((x (car args)))
       (if (and (number? x) (integer? x) (>= x 0))
           (wgsl-result :u32 '() (string-append (number->string x) "u"))
           (let ((r (wgsl x env)))
             (cond ((eq? (wgsl-type-of r) :u32) r)
                   ((eq? (wgsl-type-of r) :f32)
                    (wgsl-result :u32 (wgsl-stmts-of r)
                                 (string-append "u32(" (wgsl-code-of r) ")")))
                   (else (error 'wgsl (string-append
                          "(u32) expects a non-negative integer, an f32 or a u32, got "
                          (wgsl-type-name (wgsl-type-of r))))))))))

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
     (wgsl-arity op args 2)
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
     (wgsl-arity op args 2)
     (let ((a (wgsl (car args) env)) (b (wgsl (cadr args) env)))
       (wgsl-check-same 'dot (wgsl-type-of a) (wgsl-type-of b))
       (wgsl-result :f32 (wgsl-append-stmts (list a b))
                    (string-append "dot(" (wgsl-code-of a) ", " (wgsl-code-of b) ")"))))

    ;; (mix a b t) / (clamp x lo hi) / (smoothstep e0 e1 x)
    ((memq op '(mix clamp smoothstep))
     (wgsl-arity op args 3)
     (let* ((rs (map (lambda (a) (wgsl a env)) args))
            (t0 (wgsl-type-of (car rs))))
       (wgsl-check-same op t0 (wgsl-type-of (cadr rs)))
       ;; ONLY mix has a scalar-blend overload — mix(vecN, vecN, f32) is
       ;; in the spec, while clamp and smoothstep want all three operands
       ;; one type. Extending mix's leniency to all three accepted
       ;; (clamp v lo 0.5) here and handed the browser a shader it
       ;; rejects — the exact failure this file exists to move to Scheme.
       (let ((t2 (wgsl-type-of (caddr rs))))
         (if (not (or (eq? t2 t0)
                      (and (eq? op 'mix) (eq? t2 :f32))))
             (error 'wgsl (string-append "(" (symbol->string op)
                                         ") third argument must be "
                                         (if (eq? op 'mix)
                                             (string-append (wgsl-type-name t0)
                                                            " or f32")
                                             (wgsl-type-name t0))
                                         ", got:")
                    (wgsl-type-name t2))))
       (wgsl-result t0 (wgsl-append-stmts rs)
                    (string-append (symbol->string op) "("
                                   (wgsl-join (map wgsl-code-of rs) ", ") ")"))))

    ((assq op wgsl-vector-to-scalar)
     (wgsl-arity op args 1)
     (let ((r (wgsl (car args) env)))
       (wgsl-result :f32 (wgsl-stmts-of r)
                    (string-append (cdr (assq op wgsl-vector-to-scalar))
                                   "(" (wgsl-code-of r) ")"))))

    ((assq op wgsl-unary-same)
     (wgsl-arity op args 1)
     (let ((r (wgsl (car args) env)))
       (wgsl-result (wgsl-type-of r) (wgsl-stmts-of r)
                    (string-append (cdr (assq op wgsl-unary-same))
                                   "(" (wgsl-code-of r) ")"))))

    ;; min and max are N-ARY, as Scheme's are, folded left onto WGSL's
    ;; binary builtins: (min a b c) is min(min(a, b), c). The fold is not
    ;; a guess — it is Scheme's own definition — and it matters because a
    ;; define-dual body must mean the same thing in both languages.
    ((memq op '(min max))
     (wgsl-arity-at-least op args 2)
     (let ((rs (map (lambda (a) (wgsl a env)) args)))
       (let check ((t (wgsl-type-of (car rs))) (rest (cdr rs)))
         (if (not (null? rest))
             (check (wgsl-check-same op t (wgsl-type-of (car rest)))
                    (cdr rest))))
       (let loop ((acc (wgsl-code-of (car rs))) (rest (cdr rs)))
         (if (null? rest)
             (wgsl-result (wgsl-type-of (car rs)) (wgsl-append-stmts rs) acc)
             (loop (string-append (symbol->string op) "(" acc ", "
                                  (wgsl-code-of (car rest)) ")")
                   (cdr rest))))))

    ((assq op wgsl-binary-same)
     (wgsl-arity op args 2)
     (let ((a (wgsl (car args) env)) (b (wgsl (cadr args) env)))
       (wgsl-result (wgsl-check-same op (wgsl-type-of a) (wgsl-type-of b))
                    (wgsl-append-stmts (list a b))
                    (string-append (cdr (assq op wgsl-binary-same)) "("
                                   (wgsl-code-of a) ", " (wgsl-code-of b) ")"))))

    ;; (remainder a b) truncates — the sign follows the DIVIDEND, which is
    ;; what WGSL's % and C's fmod do. (modulo a b) floors — the sign
    ;; follows the DIVISOR, which is what Scheme's modulo and GLSL's mod
    ;; do. They agree whenever both operands are non-negative and disagree
    ;; on every case where they do not, so BOTH are spelled out and
    ;; neither is called `mod`: someone arriving from GLSL and someone
    ;; arriving from WGSL would read that name as opposite things, and a
    ;; centred grid or a noise field crosses zero constantly.
    ;;
    ;; `%` IS a synonym for remainder, and is safe where `mod` was not:
    ;; the glyph is WGSL's own spelling, so it can only mean what WGSL
    ;; means by it. The ambiguity was in the word, not the operation.
    ;;
    ;; The two diverge on a negative DIVIDEND, not a negative divisor:
    ;; (modulo -1 3) is 2 and (remainder -1 3) is -1, both with a positive
    ;; divisor. Wrapping a coordinate that has gone negative back into
    ;; [0, b) is the case, and it is the ordinary one.
    ((memq op '(remainder % modulo))
     (wgsl-arity op args 2)
     (let* ((a (wgsl (car args) env))
            (b (wgsl (cadr args) env))
            (t (wgsl-arith-type op (wgsl-type-of a) (wgsl-type-of b))))
       (if (or (memq op '(remainder %)) (eq? t :u32))
           ;; Unsigned operands cannot disagree: with nothing negative the
           ;; two definitions coincide, so the floor would be dead work.
           (wgsl-result t (wgsl-append-stmts (list a b))
                        (string-append "(" (wgsl-code-of a) " % "
                                       (wgsl-code-of b) ")"))
           ;; a - b * floor(a / b) names both operands twice, so both are
           ;; bound first — see wgsl-bind.
           (let ((ab (wgsl-bind "m" a)) (bb (wgsl-bind "n" b)))
             (wgsl-result t (append (wgsl-stmts-of ab) (wgsl-stmts-of bb))
                          (string-append "(" (wgsl-code-of ab) " - "
                                         (wgsl-code-of bb) " * floor("
                                         (wgsl-code-of ab) " / "
                                         (wgsl-code-of bb) "))"))))))

    ;; Comparisons produce bool, which exists only to feed `if` and the
    ;; boolean connectives — there is no other way to make one and nothing
    ;; else consumes one.
    ;;
    ;; N-ARY AS A CHAIN, which is Scheme's own meaning: (< lo x hi) is
    ;; lo < x AND x < hi, each operand evaluated once. It used to read
    ;; exactly two arguments and silently drop the rest, so that
    ;; idiomatic bounds check compiled to (lo < x) — accepted, plausible,
    ;; and missing its upper bound.
    ((memq op '(< > <= >= =))
     (wgsl-arity-at-least op args 2)
     (let* ((rs0 (map (lambda (a) (wgsl a env)) args))
            (t   (wgsl-type-of (car rs0))))
       ;; Scalars of the SAME type: all f32s or all u32s. WGSL will not
       ;; compare across them and neither will this, since the conversion
       ;; that would make it work is exactly the one worth writing down.
       (if (not (memq t '(:f32 :u32)))
           (error 'wgsl
                  (string-append "(" (symbol->string op)
                                 ") compares scalars, got: "
                                 (wgsl-type-name t))))
       (for-each
        (lambda (r)
          (if (not (eq? (wgsl-type-of r) t))
              (error 'wgsl
                     (string-append "(" (symbol->string op)
                                    ") compares scalars of one type, got: "
                                    (wgsl-type-name t) " and "
                                    (wgsl-type-name (wgsl-type-of r))))))
        (cdr rs0))
       ;; A MIDDLE operand appears in two comparisons, so it is bound
       ;; first — splicing its text twice would evaluate it twice, and a
       ;; draw must not run twice (the same property modulo protects).
       ;; The ends are mentioned once and stay as they are, so a
       ;; two-argument comparison emits exactly what it always did.
       (let* ((rs (let loop ((xs rs0) (first #t) (acc '()))
                    (cond ((null? xs) (reverse acc))
                          ((or first (null? (cdr xs)))
                           (loop (cdr xs) #f (cons (car xs) acc)))
                          (else
                           (loop (cdr xs) #f
                                 (cons (wgsl-bind "cmp" (car xs)) acc))))))
              (sym (if (eq? op '=) "==" (symbol->string op)))
              (pairs (let loop ((xs rs) (acc '()))
                       (if (null? (cdr xs))
                           (reverse acc)
                           (loop (cdr xs)
                                 (cons (string-append
                                        "(" (wgsl-code-of (car xs)) " " sym " "
                                        (wgsl-code-of (cadr xs)) ")")
                                       acc))))))
         (wgsl-result :bool (wgsl-append-stmts rs)
                      (if (null? (cdr pairs))
                          (car pairs)
                          (string-append "(" (wgsl-join pairs " && ") ")"))))))

    ;; N-ary, folded left like arithmetic: (and a b c) is ((a && b) && c).
    ;; It used to read exactly two operands and silently drop the rest —
    ;; a three-way guard that quietly lost its third conjunct.
    ((memq op '(and or))
     (wgsl-arity-at-least op args 2)
     (let ((rs (map (lambda (a) (wgsl a env)) args)))
       (for-each
        (lambda (r)
          (if (not (eq? (wgsl-type-of r) :bool))
              (error 'wgsl (string-append "(" (symbol->string op)
                                          ") needs bool operands, got: "
                                          (wgsl-type-name (wgsl-type-of r))))))
        rs)
       (let loop ((acc (wgsl-code-of (car rs))) (rest (cdr rs)))
         (if (null? rest)
             (wgsl-result :bool (wgsl-append-stmts rs) acc)
             (loop (string-append "(" acc
                                  (if (eq? op 'and) " && " " || ")
                                  (wgsl-code-of (car rest)) ")")
                   (cdr rest))))))

    ((eq? op 'not)
     (wgsl-arity op args 1)
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
     (wgsl-arity-at-least op args 1)
     (if (null? (cdr args))
         ;; Scheme's unary meanings, kept exactly: (- a) negates, (/ a)
         ;; is the RECIPROCAL, (+ a) and (* a) are a. The reciprocal used
         ;; to come back as the operand itself — accepted, plausible, and
         ;; wrong by a power of minus one.
         (let ((r (wgsl (car args) env)))
           (cond ((eq? op '-)
                  (wgsl-result (wgsl-type-of r) (wgsl-stmts-of r)
                               (string-append "-(" (wgsl-code-of r) ")")))
                 ((eq? op '/)
                  (wgsl-result (wgsl-arith-type op :f32 (wgsl-type-of r))
                               (wgsl-stmts-of r)
                               (string-append "(1.0 / " (wgsl-code-of r) ")")))
                 (else r)))
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

    ;; TERMINAL FORMS. A form that writes rather than evaluates — the last
    ;; thing a body does, and the only place statements enter this language.
    ;;
    ;; Registered rather than built in, because the compiler has no
    ;; business knowing what a point is. lib/wrangle.scm owns that concept
    ;; and installs the handler; this file only knows that some forms end a
    ;; body instead of producing a value.
    ((map-ref wgsl-terminals op)
     => (lambda (handler) (handler args env)))

    (else (error 'wgsl "unknown operator in kernel:" op))))

;; name -> handler, where a handler takes (args env) and returns a
;; wgsl-result whose type is :point.
;;
;; GUARDED like the tables below — and it was the one that was not. A
;; re-load of this file wiped it, so the ordering "load wrangle, then
;; something that transitively re-loads wgsl, then compile a kernel"
;; lost the `point` terminal: the shim-outlives-the-layout bug seen from
;; the other side. The suite's load order happened to dodge it.
(define-once wgsl-terminals {})

(define (wgsl-define-terminal! name handler)
  (map-set! wgsl-terminals name handler))

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

;; name -> (scheme-name wgsl-name (arg-type ...) result-type). The entry
;; keeps its name in front, so a positional reader of one sees exactly
;; the alist entry these tables used to hold.
;;
;; THE REGISTRIES ARE MAPS, and the choice is semantic rather than
;; fashionable: an ObjMap is an insertion-ordered association vector
;; whose map-set! on an existing key replaces IN PLACE — precisely the
;; replace-rather-than-shadow every one of these tables hand-rolled,
;; needed because watch mode re-runs a file on every save and a table
;; that only ever grew would accumulate a stale entry per save. Keys
;; compare by identity, which for interned symbols is assq's question.
;;
;; GUARDED, because a second (load "lib/wgsl.scm") would otherwise reset
;; this to empty and silently discard every signature registered since the
;; first — and transitive double-loading is the normal case once more than
;; one library wants the kernel compiler. The definitions and duals tables
;; below are guarded for the same reason.
(define-once wgsl-signatures {})

(define (wgsl-declare! name wgsl-name arg-types result-type)
  (map-set! wgsl-signatures name (list name wgsl-name arg-types result-type)))

(define (wgsl-signature name) (map-ref wgsl-signatures name))

;; Take a declaration back. A DECLARATION is a promise that hand-written
;; WGSL of that name exists, and layer 18 checks every promise against the
;; assembled shader — so a test that declares a stub purely to exercise a
;; call site has made a promise it cannot keep and must withdraw it.
;;
;; Before the load guard above, re-loading this file wiped the table, and
;; that accident was doing the withdrawing. Relying on it was never a plan.
(define (wgsl-forget-declaration! name)
  (map-delete! wgsl-signatures name))

;; Scheme spells names with hyphens, WGSL with underscores.
(define (wgsl-fn-name name) (wgsl-underscore (symbol->string name)))

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

;; name -> source, in emission order. The map KEEPS that order —
;; map-values walks insertion order, and a replacement holds its
;; original position — and both halves are load-bearing: a function
;; must appear before the code that calls it, and a redefinition must
;; not migrate to the end. Guarded — see wgsl-signatures.
(define-once wgsl-definitions {})

(define (wgsl-put-definition! name source)
  (map-set! wgsl-definitions name source))

(define (wgsl-definitions-source)
  (wgsl-join (map-values wgsl-definitions) "\n"))

(define (wgsl-forget-definitions!)
  (set! wgsl-definitions {}))

;; Take ONE definition back. The blanket version above cannot be used for
;; this: the table holds library definitions registered at load time and
;; program definitions registered at run time, and only the second kind
;; ever goes stale — see shared-layout!.
(define (wgsl-forget-definition! name)
  (map-delete! wgsl-definitions name))

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
                        (string-append (wgsl-underscore (symbol->string (car p))) " : "
                                       (wgsl-type-name (cadr p))))
                      params)
                 ", ")
      ") -> " (wgsl-type-name ret) " {\n"
      (wgsl-join (map (lambda (l) (string-append "  " l)) lines) "\n")
      "\n}\n"))
    (wgsl-declare! name wname (map cadr params) ret)
    ;; After the compile, so a body that does not type-check is not kept
    ;; as though it were a citizen.
    (wgsl-put-body! name params body)
    ret))

;; (define-gpu (name (arg type) ...) body)
;;
;; A macro for the same reason define-kernel is one: the body is code in
;; another language and must not be evaluated as Scheme. The result type is
;; deliberately NOT declared — deriving it is what makes the signature
;; honest rather than an assertion that could drift from the body.
(defmacro (define-gpu spec body)
  `(wgsl-define-fn! ',(car spec) ',(cdr spec) ',body))

;; (define-dual (name (arg type) ...) body)
;;
;; ONE definition, two citizenships: an ordinary Scheme procedure the VM
;; runs, and a kernel function a call site can reach. Written once, so the
;; two cannot drift AT THE SOURCE — which is the whole tax a function pays
;; to enter the kernel domain, and the reason the tax is cheap.
;;
;; Note the asymmetry in the expansion, because it is the entire idea: the
;; CPU side splices the body UNQUOTED, so Scheme evaluates it; the kernel
;; side splices it QUOTED, so the compiler reads it as text in another
;; language. Same datum, two fates, no second copy to maintain.
;;
;; What this does NOT buy is NUMERICAL agreement: the device computes in
;; f32 and the VM in f64. That gap is deliberate and load-bearing — see
;; lib/dist.scm's header on why the more precise side is the oracle — so a
;; comparison across it is a measurement, not a test to be tightened until
;; it passes.
;;
;; The two halves fail at different moments, which is worth knowing when
;; one does: the kernel half is compiled and type-checked HERE, at
;; definition, while the Scheme half is only read here and can still carry
;; an unbound name (a kernel builtin with no Scheme meaning) that surfaces
;; on the first call.
;; The Scheme halves, by name. A DECLARED function has none — nothing can
;; run hand-written WGSL on the VM — so this table is exactly the set of
;; names that mean something in both worlds, which is the question anything
;; evaluating kernel code on the host needs answered.
(define-once wgsl-duals {})

(define (wgsl-put-dual! name proc)
  (map-set! wgsl-duals name proc))

;; The procedure itself, or #f. It used to be the (name . proc) alist
;; entry, and every caller immediately took the cdr.
(define (wgsl-dual name) (map-ref wgsl-duals name))

;; The BODIES, by name — kept for the same reason define-gen keeps a
;; model's source, and it is the same argument one level down. A helper
;; that entered the kernel domain handed its body over as a datum already
;; (define-gpu and define-dual both splice it quoted); compiling it to
;; text and dropping the datum throws away the only readable statement of
;; what the function MEANS.
;;
;; What needs it: a call is opaque in the staged IR, so
;; (call curve-elem (data xs j) (choice :a) ...) hides whether the mean is
;; affine in :a — which is exactly the question a conjugate update turns
;; on. Reading the body answers it by proof. The alternative is to
;; evaluate the helper at two points and fit a line, which is unsound for
;; the same reason probing a model for its address sequence is: a
;; quadratic passes through any two points, and a wrong update would be
;; inherited identically by both backends, so the oracle could not catch
;; it.
;;
;; The cost is nothing. This table holds one s-expression per function
;; that paid the entry tax, and the tax is what bounds the table: the set
;; of bodies kept is exactly the set of names a kernel may call.
(define-once wgsl-bodies {})

(define (wgsl-put-body! name params body)
  (map-set! wgsl-bodies name (cons params body)))

;; (params . body), or #f for a DECLARED function, which has no body in
;; this language to read.
(define (wgsl-fn-body name) (map-ref wgsl-bodies name))

(define (wgsl-body-params b) (car b))
(define (wgsl-body-expr b) (cdr b))

(define (wgsl-forget-body! name)
  (map-delete! wgsl-bodies name))

;; Does a retained body mention this name anywhere? Crude on purpose: a
;; call is the case that matters and a shadowing binding of the same name
;; would only cost a definition that is cheap to re-register.
(define (wgsl-body-mentions? form name)
  (cond ((symbol? form) (eq? form name))
        ((pair? form) (or (wgsl-body-mentions? (car form) name)
                          (wgsl-body-mentions? (cdr form) name)))
        (else #f)))

;; Retract every definition that CALLS one of NAMES, and the declaration
;; that promises it.
;;
;; A definition is only valid while everything it calls is. `define-gpu`
;; registers into a table that OUTLIVES the program that wrote it, so a
;; body calling something with a shorter lifetime -- a shared accessor is
;; the only such thing -- has to come back out when that lifetime ends,
;; or it is emitted into the next program's module calling a function
;; nothing defines.
;;
;; ONE level deep, which is every shim that exists: they all call an
;; accessor directly. A define-gpu calling a shim rather than an accessor
;; would survive this and want a fixpoint instead.
(define (wgsl-forget-dependents! names)
  ;; map-keys is a snapshot, so forgetting mid-walk cannot trip the walk.
  (for-each
   (lambda (name)
     (let ((body (cdr (wgsl-fn-body name))))
       (if (let check ((ns names))
             (cond ((null? ns) #f)
                   ((wgsl-body-mentions? body (car ns)) #t)
                   (else (check (cdr ns)))))
           (begin (wgsl-forget-definition! name)
                  (wgsl-forget-body! name)
                  (wgsl-forget-declaration! name)))))
   (map-keys wgsl-bodies)))

(defmacro (define-dual spec body)
  `(begin
     (define (,(car spec) ,@(map car (cdr spec))) ,body)
     (wgsl-define-fn! ',(car spec) ',(cdr spec) ',body)
     (wgsl-put-dual! ',(car spec) ,(car spec))))

;;--- shared read-only data ----------------------------------------------
;; Data every invocation reads, rather than data each invocation owns.
;; Lives here rather than in one harness because BOTH want it: a compute
;; wrangle reads observations, and a fragment kernel reads — for instance
;; — a table of particles it is drawing. Only the binding number differs,
;; so only that is a parameter.
;;
;; DECLARED, not dynamic. A GPU buffer cannot be made up as it goes, since
;; allocation precedes dispatch. So the declaration is the single source of
;; truth and generates the accessors for both sides, rather than asking
;; anyone to agree with an offset by hand — an offset computed in two
;; places is one that will eventually disagree with itself, and the failure
;; is a plausible wrong picture rather than an error.
;;
;;   (shared-layout! '((walls 48) (obs 41)))   ; then (shared-obs k)
;;
;; The WGSL identifier is `sdata`, not `shared` — `shared` is a reserved
;; word there and a binding named that will not compile.
;; DELIBERATELY NOT GUARDED, unlike the signature and definition tables
;; above, and the distinction is worth stating because it is easy to get
;; backwards. Those tables are populated DURING load — define-dual in
;; lib/dist.scm registers into them — so a second load must not reset them
;; or the registrations vanish. A layout is declared at RUN time, by a
;; program, and never by a library at load time. So re-loading must reset
;; it: that is how a demo gets a clean slate instead of inheriting the
;; regions of whichever one ran before it, which would emit a binding
;; nothing binds. testcases/test_gpu_presets.js checks exactly that, and
;; caught this the first time it was guarded.
(define shared-regions '())    ; ((name offset length) ...)
(define shared-length 0)       ; total floats

(define (shared-layout! specs)
  ;; Retract the previous layout's accessors first. A declaration is a
  ;; promise that a function of that name is in the module, and replacing
  ;; the layout is exactly what stops the old ones being emitted — so
  ;; leaving them declared would promise functions nothing defines, which
  ;; is what layer 18 checks for and what a browser reports as an
  ;; unresolved call target.
  ;; And retract whatever CALLED them. A define-gpu shim exists precisely
  ;; to bridge an accessor's name to a kernel's, so it dies with the
  ;; layout too -- otherwise it survives in the definition table and is
  ;; emitted into the next program's module, which is the same unresolved
  ;; call target seen from the other side.
  (let ((accessors
         (map (lambda (r)
                (string->symbol (string-append "shared-" (symbol->string (car r)))))
              shared-regions)))
    (for-each wgsl-forget-declaration! accessors)
    (wgsl-forget-dependents! accessors))
  (let loop ((ss specs) (off 0) (acc '()))
    (if (null? ss)
        (begin
          (set! shared-regions (reverse acc))
          (set! shared-length off)
          (for-each
           (lambda (r)
             (wgsl-declare! (string->symbol (string-append "shared-"
                                                           (symbol->string (car r))))
                            (string-append "shared_" (wgsl-fn-name (car r)))
                            (list :u32) :f32))
           shared-regions)
          shared-length)
        (let ((spec (car ss)))
          (if (or (not (pair? spec)) (not (pair? (cdr spec)))
                  (not (symbol? (car spec)))
                  (not (integer? (cadr spec))) (< (cadr spec) 1))
              (error "shared-layout!: expected (name length), length >= 1" spec))
          (loop (cdr ss) (+ off (cadr spec))
                (cons (list (car spec) off (cadr spec)) acc))))))

(define (shared-region name)
  (let loop ((rs shared-regions))
    (cond ((null? rs) (error "shared: undeclared region" name))
          ((eq? (caar rs) name) (car rs))
          (else (loop (cdr rs))))))

(define (shared-offset name) (cadr (shared-region name)))
(define (shared-size name) (caddr (shared-region name)))

;; Emitted only when something is declared, so a kernel that reads no
;; shared data keeps exactly the bind group layout it had.
(define (shared-preamble binding)
  (if (null? shared-regions)
      ""
      (apply string-append
             (string-append "@group(0) @binding(" (number->string binding)
                            ") var<storage, read> sdata : array<f32>;\n")
             (map (lambda (r)
                    (string-append
                     "fn shared_" (wgsl-fn-name (car r)) "(k : u32) -> f32 { return sdata["
                     (number->string (cadr r)) "u + k]; }\n"))
                  shared-regions))))

(define (make-shared)
  (let ((b (make-bytes (* shared-length 4))))
    (bytes-seal! b)
    b))

(define (shared-view b) (bytes-view b :f32))

;;--- entry points -------------------------------------------------------

;; Compile a self-contained expression. Resets the local counter so the
;; emitted text depends only on the expression, which is what makes it
;; testable by string comparison.
(define (wgsl-compile expr env)
  (set! wgsl-counter 0)
  (wgsl expr env))

;; Compile a SUB-expression of one already being compiled: same rules, no
;; reset. The distinction matters where several expressions are compiled
;; into ONE function body, because locals are hoisted to function scope
;; and the counter is the only thing keeping them apart.
;;
;; What went wrong without it: a terminal form compiles each of its
;; arguments through wgsl-compile, so every attribute expression restarted
;; the counter, and two `fold-i` forms in one body both emitted `var
;; acc_2` — one f32 and one vec4<f32>, with the first reader silently
;; getting the second fold's value. The property the reset exists for
;; still holds, just per KERNEL rather than per argument: the emitted text
;; depends only on the expression compiled at the top.
(define (wgsl-nested expr env) (wgsl expr env))

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

;; Registered at load, so they are FIRST in emission order and therefore
;; declared before anything that calls them — WGSL has no forward
;; declarations. Idempotent on a reload, since wgsl-put-definition!
;; replaces by name and keeps the position.
(wgsl-register-nonfinite-helpers!)
