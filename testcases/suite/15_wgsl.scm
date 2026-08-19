;;----------------------------------------------------------------------
;; Layer 15: the Scheme -> WGSL kernel compiler (lib/wgsl.scm)
;;
;; VEX and Shadertoy both reduce to a pure per-element kernel — pixel to
;; colour, point to attributes — so the kernel compiler is the part that
;; serves both, and it is being built first, alone, with no GPU attached.
;; That is deliberate: emitting WGSL is producing a string, and a string
;; can be asserted against here rather than squinted at in a browser.
;;
;; Half of these tests are for REJECTION, which is the whole reason this
;; is a type checker instead of a template. WGSL does not coerce, and a
;; type error that escapes to the GPU comes back as a shader compilation
;; log in a browser console — no line number in our source, no stack, no
;; way to bisect. Every mismatch caught here is one that would otherwise
;; be debugged there.
;;
;; The one rule resting on the language rather than on a test here is
;; scalar-against-vector broadcast (`v * 2.0`), since nothing in this file
;; compiles WGSL. It is what the spec says and it holds in practice.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")
(load "lib/wgsl.scm")

(test-suite "15_wgsl: typed Scheme-to-WGSL kernel compiler")

(define E '((uv . vec2f) (time . f32) (col . vec3f) (p . vec4f)))

;; Catch a wgsl compile error and hand back its message, so rejection can
;; be asserted as ordinary data.
(define (wgsl-error expr)
  (guard (e (#t 'rejected))
    (wgsl-code expr E)
    'accepted))

;;--- literals and variables ---------------------------------------------
;; Every literal carries a decimal point. An emitted "1" would be i32 in
;; WGSL and would fail to add to an f32 — the single most likely way for
;; generated shader source to break.

(assert-equal "an integer literal emits as f32" "1.0" (wgsl-code '1 E))
(assert-equal "zero emits as f32" "0.0" (wgsl-code '0 E))
(assert-equal "a negative literal" "-2.0" (wgsl-code '-2 E))
(assert-equal "a float literal is left alone" "2.5" (wgsl-code '2.5 E))
(assert-equal "a variable emits its name" "uv" (wgsl-code 'uv E))
(assert-equal "a variable carries its type" 'vec2f (wgsl-type 'uv E))
(assert-equal "an unbound variable is rejected" 'rejected (wgsl-error 'nope))

;;--- arithmetic and broadcast -------------------------------------------

(assert-equal "addition" "(1.0 + 2.5)" (wgsl-code '(+ 1 2.5) E))
(assert-equal "left-folded n-ary addition"
              "((1.0 + 2.0) + 3.0)" (wgsl-code '(+ 1 2 3) E))
(assert-equal "subtraction" "(uv - 0.5)" (wgsl-code '(- uv 0.5) E))
(assert-equal "unary minus" "-(time)" (wgsl-code '(- time) E))
(assert-equal "division" "(time / 2.0)" (wgsl-code '(/ time 2) E))

(assert-equal "scalar times vector is a vector" 'vec2f (wgsl-type '(* uv 2.0) E))
(assert-equal "vector times scalar is a vector" 'vec2f (wgsl-type '(* 2.0 uv) E))
(assert-equal "scalar plus scalar stays scalar" 'f32 (wgsl-type '(+ time 1) E))
(assert-equal "matching vectors combine" 'vec3f (wgsl-type '(+ col col) E))

;; The rejection that matters most: mixing widths. WGSL has no rule for
;; this and neither should we.
(assert-equal "vec2 + vec3 is rejected" 'rejected (wgsl-error '(+ uv col)))
(assert-equal "vec3 * vec4 is rejected" 'rejected (wgsl-error '(* col p)))

;;--- constructors -------------------------------------------------------

(assert-equal "vec3 constructor"
              "vec3<f32>(1.0, 2.0, 3.0)" (wgsl-code '(vec3 1 2 3) E))
(assert-equal "vec2 constructor uses the long type name"
              "vec2<f32>(time, 0.0)" (wgsl-code '(vec2 time 0) E))
(assert-equal "vec4 constructor" 'vec4f (wgsl-type '(vec4 1 2 3 4) E))
(assert-equal "constructors nest"
              "vec3<f32>(sin(time), 0.2, 0.8)"
              (wgsl-code '(vec3 (sin time) 0.2 0.8) E))

(assert-equal "too few components is rejected" 'rejected (wgsl-error '(vec3 1 2)))
(assert-equal "too many components is rejected" 'rejected (wgsl-error '(vec2 1 2 3)))
(assert-equal "a vector component is rejected" 'rejected (wgsl-error '(vec3 uv 1 2)))

;;--- built-ins ----------------------------------------------------------

(assert-equal "a unary function keeps its type" 'vec3f (wgsl-type '(sin col) E))
(assert-equal "unary emission" "sqrt(time)" (wgsl-code '(sqrt time) E))
(assert-equal "length takes a vector to a scalar" 'f32 (wgsl-type '(length uv) E))
(assert-equal "length emission"
              "length((uv - 0.5))" (wgsl-code '(length (- uv 0.5)) E))
(assert-equal "dot takes two vectors to a scalar" 'f32 (wgsl-type '(dot uv uv) E))
(assert-equal "dot of mismatched vectors is rejected"
              'rejected (wgsl-error '(dot uv col)))

(assert-equal "min of matching types" "min(time, 1.0)" (wgsl-code '(min time 1) E))
(assert-equal "min of mismatched types is rejected"
              'rejected (wgsl-error '(min time uv)))

(assert-equal "mix with a scalar blend factor" 'vec3f
              (wgsl-type '(mix col col time) E))
(assert-equal "mix emission"
              "mix(col, col, time)" (wgsl-code '(mix col col time) E))
(assert-equal "mix with mismatched endpoints is rejected"
              'rejected (wgsl-error '(mix col uv time)))
(assert-equal "clamp" "clamp(time, 0.0, 1.0)" (wgsl-code '(clamp time 0 1) E))
(assert-equal "smoothstep" 'f32 (wgsl-type '(smoothstep 0 1 time) E))

;;--- swizzles -----------------------------------------------------------

(assert-equal "a single component is a scalar" 'f32 (wgsl-type '(swizzle col x) E))
(assert-equal "a two-component swizzle is a vec2" 'vec2f
              (wgsl-type '(swizzle col xy) E))
(assert-equal "swizzle emission" "col.xy" (wgsl-code '(swizzle col xy) E))
(assert-equal "a swizzle can widen" 'vec4f (wgsl-type '(swizzle col xyzx) E))
;; .z on a vec2 is the error a hand-written shader makes constantly.
(assert-equal "a component past the end is rejected"
              'rejected (wgsl-error '(swizzle uv z)))
(assert-equal "a non-component letter is rejected"
              'rejected (wgsl-error '(swizzle col q)))

;;--- let ----------------------------------------------------------------
;; WGSL has no block expression, so a binding is hoisted into a statement
;; and the body refers to it by a NUMBERED name. The declaration and the
;; use have to agree — when they first didn't, the emitted shader declared
;; d_1 and returned an expression mentioning d, which WGSL would either
;; reject or, worse, silently resolve to some other d in scope.

(assert-equal "a let hoists a statement and renames the reference"
              (string-append "  let d_1 : f32 = length((uv - 0.5));\n"
                             "  return vec3<f32>(sin(((d_1 * 10.0) + time)), 0.2, 0.8);")
              (wgsl-body '(let ((d (length (- uv 0.5))))
                            (vec3 (sin (+ (* d 10.0) time)) 0.2 0.8))
                         E "  "))

(assert-equal "a let binding carries its type into the body" 'vec2f
              (wgsl-type '(let ((q (* uv 2.0))) q) E))

(assert-equal "sequential lets each get their own name"
              (string-append "  let a_1 : f32 = (time * 2.0);\n"
                             "  let b_2 : f32 = (a_1 + 1.0);\n"
                             "  return (a_1 + b_2);")
              (wgsl-body '(let ((a (* time 2.0)) (b (+ a 1))) (+ a b)) E "  "))

;; Shadowing survives the flattening into one WGSL scope, because the
;; numbering makes the inner binding a different name.
(assert-equal "an inner let shadows an outer one"
              (string-append "  let x_1 : f32 = 1.0;\n"
                             "  let x_2 : f32 = 2.0;\n"
                             "  return x_2;")
              (wgsl-body '(let ((x 1)) (let ((x 2)) x)) E "  "))

(assert-equal "a type error inside a let is still caught"
              'rejected (wgsl-error '(let ((bad (+ uv col))) bad)))

;; The counter resets per compile, so emitted text depends only on the
;; expression — otherwise these assertions would depend on test order.
(assert-equal "compilation is reproducible"
              (wgsl-body '(let ((d 1)) d) E "")
              (wgsl-body '(let ((d 1)) d) E ""))

;;--- a whole kernel body ------------------------------------------------

(assert-equal "a plasma-ish kernel compiles end to end"
              (string-append
               "  let c_1 : vec2<f32> = (uv - 0.5);\n"
               "  let r_2 : f32 = length(c_1);\n"
               "  let w_3 : f32 = sin(((r_2 * 12.0) - time));\n"
               "  return vec3<f32>(w_3, (w_3 * 0.5), 0.8);")
              (wgsl-body '(let ((c (- uv 0.5))
                                (r (length c))
                                (w (sin (- (* r 12.0) time))))
                            (vec3 w (* w 0.5) 0.8))
                         E "  "))

(suite-summary)
