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

(define E '((uv . :vec2f) (time . :f32) (col . :vec3f) (p . :vec4f)))

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
(assert-equal "a variable carries its type" :vec2f (wgsl-type 'uv E))
(assert-equal "an unbound variable is rejected" 'rejected (wgsl-error 'nope))

;;--- arithmetic and broadcast -------------------------------------------

(assert-equal "addition" "(1.0 + 2.5)" (wgsl-code '(+ 1 2.5) E))
(assert-equal "left-folded n-ary addition"
              "((1.0 + 2.0) + 3.0)" (wgsl-code '(+ 1 2 3) E))
(assert-equal "subtraction" "(uv - 0.5)" (wgsl-code '(- uv 0.5) E))
(assert-equal "unary minus" "-(time)" (wgsl-code '(- time) E))
(assert-equal "division" "(time / 2.0)" (wgsl-code '(/ time 2) E))

;;--- the two modular operators ------------------------------------------
;; They are DIFFERENT operations and both are spelled out, because the two
;; languages a reader is likely to be arriving from disagree about which
;; one `mod` means. WGSL's % truncates (sign follows the dividend, as C's
;; fmod); GLSL's mod floors (sign follows the divisor, as Scheme's modulo).
;; On non-negative operands they agree, which is exactly why picking one
;; and calling it `mod` would survive early testing and then be wrong on a
;; centred grid. They diverge on a negative DIVIDEND, not a negative
;; divisor: (modulo -1 3) is 2 and (remainder -1 3) is -1, both with a
;; positive divisor — wrapping a coordinate that has gone negative back
;; into [0, b) is the ordinary case, not an exotic one.
(define EU '((i . :u32) (n . :u32) (time . :f32)))
(assert-equal "remainder is WGSL's %, straight through"
              "(time % 2.0)" (wgsl-code '(remainder time 2) E))
(assert-equal "and on unsigned operands too"
              "(i % n)" (wgsl-code '(remainder i n) EU))
;; `%` is a synonym, and safe where `mod` was not: the glyph is WGSL's own
;; spelling, so it cannot be read as GLSL's floored operation.
(assert-equal "% is the same operator by its WGSL spelling"
              "(time % 2.0)" (wgsl-code '(% time 2) E))
;; With nothing negative the two definitions coincide, so the floor would
;; be dead work — an unsigned modulo emits the same % and needs no
;; statements at all.
(assert-equal "unsigned modulo needs no floor"
              "(i % n)" (wgsl-code '(modulo i n) EU))

;; The floored form names both operands twice, so both are bound first.
;; Splicing the operand text twice would EVALUATE it twice, and the
;; generator is stateful: (modulo (random-normal ...) k) would draw two
;; different numbers and combine them. This is the property that assertion
;; protects, and it is invisible in the result type.
(assert-equal "a float modulo floors, binding both operands"
              (string-append "let m_1 : f32 = time;\n"
                             "let n_2 : f32 = 2.0;\n"
                             "return (m_1 - n_2 * floor(m_1 / n_2));")
              (wgsl-body '(modulo time 2) E ""))
;; A stand-in for the effectful calls the real environment supplies —
;; lib/wrangle.scm's generator lives a layer up, and the property being
;; asserted belongs to the compiler, not to that generator.
(wgsl-declare! 'draw "draw_one" '(:f32) :f32)
(define (occurrences hay needle)
  (let ((h (string-length hay)) (n (string-length needle)))
    (let loop ((k 0) (c 0))
      (cond ((> (+ k n) h) c)
            ((string=? (substring hay k (+ k n)) needle) (loop (+ k 1) (+ c 1)))
            (else (loop (+ k 1) c))))))
(assert-equal "so an operand that draws is drawn once"
              1 (occurrences (wgsl-body '(modulo (draw time) 2) E "") "draw_one("))
(assert-equal "where naming it twice would have drawn twice"
              2 (occurrences (wgsl-body '(- (draw time) (* 2 (floor (/ (draw time) 2)))) E "")
                             "draw_one("))

;; Mixed signedness is refused here as everywhere: WGSL will not, and the
;; conversion that would make it work is the one worth writing.
(assert-equal "a float cannot be taken modulo an integer"
              'rejected (guard (e (#t 'rejected))
                          (wgsl-body '(modulo time n) EU "") 'accepted))

(assert-equal "scalar times vector is a vector" :vec2f (wgsl-type '(* uv 2.0) E))
(assert-equal "vector times scalar is a vector" :vec2f (wgsl-type '(* 2.0 uv) E))
(assert-equal "scalar plus scalar stays scalar" :f32 (wgsl-type '(+ time 1) E))
(assert-equal "matching vectors combine" :vec3f (wgsl-type '(+ col col) E))

;; The rejection that matters most: mixing widths. WGSL has no rule for
;; this and neither should we.
(assert-equal "vec2 + vec3 is rejected" 'rejected (wgsl-error '(+ uv col)))
(assert-equal "vec3 * vec4 is rejected" 'rejected (wgsl-error '(* col p)))

;;--- constructors -------------------------------------------------------

(assert-equal "vec3 constructor"
              "vec3<f32>(1.0, 2.0, 3.0)" (wgsl-code '(vec3 1 2 3) E))
(assert-equal "vec2 constructor uses the long type name"
              "vec2<f32>(time, 0.0)" (wgsl-code '(vec2 time 0) E))
(assert-equal "vec4 constructor" :vec4f (wgsl-type '(vec4 1 2 3 4) E))
(assert-equal "constructors nest"
              "vec3<f32>(sin(time), 0.2, 0.8)"
              (wgsl-code '(vec3 (sin time) 0.2 0.8) E))

(assert-equal "too few components is rejected" 'rejected (wgsl-error '(vec3 1 2)))
(assert-equal "too many components is rejected" 'rejected (wgsl-error '(vec2 1 2 3)))
(assert-equal "a vector component is rejected" 'rejected (wgsl-error '(vec3 uv 1 2)))

;;--- built-ins ----------------------------------------------------------

(assert-equal "a unary function keeps its type" :vec3f (wgsl-type '(sin col) E))
(assert-equal "unary emission" "sqrt(time)" (wgsl-code '(sqrt time) E))
(assert-equal "length takes a vector to a scalar" :f32 (wgsl-type '(length uv) E))
(assert-equal "length emission"
              "length((uv - 0.5))" (wgsl-code '(length (- uv 0.5)) E))
(assert-equal "dot takes two vectors to a scalar" :f32 (wgsl-type '(dot uv uv) E))
(assert-equal "dot of mismatched vectors is rejected"
              'rejected (wgsl-error '(dot uv col)))

(assert-equal "min of matching types" "min(time, 1.0)" (wgsl-code '(min time 1) E))
(assert-equal "min of mismatched types is rejected"
              'rejected (wgsl-error '(min time uv)))

(assert-equal "mix with a scalar blend factor" :vec3f
              (wgsl-type '(mix col col time) E))
(assert-equal "mix emission"
              "mix(col, col, time)" (wgsl-code '(mix col col time) E))
(assert-equal "mix with mismatched endpoints is rejected"
              'rejected (wgsl-error '(mix col uv time)))
(assert-equal "clamp" "clamp(time, 0.0, 1.0)" (wgsl-code '(clamp time 0 1) E))
(assert-equal "smoothstep" :f32 (wgsl-type '(smoothstep 0 1 time) E))

;;--- swizzles -----------------------------------------------------------

(assert-equal "a single component is a scalar" :f32 (wgsl-type '(swizzle col x) E))
(assert-equal "a two-component swizzle is a vec2" :vec2f
              (wgsl-type '(swizzle col xy) E))
(assert-equal "swizzle emission" "col.xy" (wgsl-code '(swizzle col xy) E))
(assert-equal "a swizzle can widen" :vec4f (wgsl-type '(swizzle col xyzx) E))
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

(assert-equal "a let binding carries its type into the body" :vec2f
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

;;--- conditionals -------------------------------------------------------
;; Without these, every kernel this language can express is a smooth
;; gradient — there is no way to make an edge. `bool` exists only to feed
;; `if` and the connectives; nothing else produces or consumes one.
;;
;; (if c a b) becomes select(b, a, c) — note the reversed order, which is
;; select's own signature. select is BRANCHLESS: both arms are evaluated,
;; nothing short-circuits. Right on a GPU, where a real branch diverges the
;; warp, but it means an arm can never guard the other from a bad value.

(assert-equal "a comparison produces bool" :bool (wgsl-type '(> time 1) E))
(assert-equal "comparison emission" "(time > 1.0)" (wgsl-code '(> time 1) E))
(assert-equal "equality spells itself ==" "(time == 1.0)" (wgsl-code '(= time 1) E))
(assert-equal "if compiles to select with the arms swapped"
              "select(0.0, 1.0, (time > 1.0))"
              (wgsl-code '(if (> time 1) 1.0 0.0) E))
(assert-equal "if takes the type of its arms" :vec3f
              (wgsl-type '(if (> time 1) col col) E))
(assert-equal "and emission"
              "((time > 1.0) && (time < 2.0))"
              (wgsl-code '(and (> time 1) (< time 2)) E))
(assert-equal "or emission"
              "((time > 1.0) || (time < 0.0))"
              (wgsl-code '(or (> time 1) (< time 0)) E))
(assert-equal "not emission" "!((time > 1.0))" (wgsl-code '(not (> time 1)) E))

;; bool is not a number, and a non-bool is not a condition.
(assert-equal "arithmetic on bool is rejected" 'rejected
              (wgsl-error '(+ (> time 1) 1)))
(assert-equal "a scalar condition is rejected" 'rejected (wgsl-error '(if time 1 2)))
(assert-equal "comparing vectors is rejected" 'rejected (wgsl-error '(> uv uv)))
(assert-equal "mismatched if arms are rejected" 'rejected
              (wgsl-error '(if (> time 1) col uv)))
(assert-equal "non-bool operands to and are rejected" 'rejected
              (wgsl-error '(and time time)))

;;--- let is orthodox, and says so when it isn't --------------------------
;; vxs's own `let` also accepts the flat vector form (let [a 1 b 2] ...).
;; A kernel does not, deliberately — but it used to fall through to
;; (car (car bindings)) and report
;;
;;   car: contract violation, expected pair, got [c (- uv 0.5) r ...]
;;
;; naming neither the form at fault, nor the language it was in, nor what
;; that language wanted. This compiler exists to turn GPU-time failures
;; into legible compile-time ones, so leaking an internal contract
;; violation is a defect in its own terms.

(assert-equal "the flat vector binding form is rejected" 'rejected
              (wgsl-error '(let [c (- uv 0.5)] c)))
(assert-equal "a non-list binding form is rejected" 'rejected
              (wgsl-error '(let 5 1)))
(assert-equal "a binding that is not a pair is rejected" 'rejected
              (wgsl-error '(let ((a 1) b) a)))
(assert-equal "a three-element binding is rejected" 'rejected
              (wgsl-error '(let ((a 1 2)) a)))
(assert-equal "a non-symbol binding name is rejected" 'rejected
              (wgsl-error '(let ((5 1)) 5)))

;; A kernel has no sequencing, so a second body form would be silently
;; discarded rather than evaluated — worth an error, not a shrug.
(assert-equal "extra body forms are rejected" 'rejected
              (wgsl-error '(let ((a 1)) a a)))
(assert-equal "a single body form is still fine" :f32
              (wgsl-type '(let ((a 1)) (+ a 1)) E))

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

;;--- joining is linear, and must stay exact ------------------------------
;; wgsl-join is the one function every emitter goes through. It used to
;; fold string-append recursively, copying the whole accumulated tail at
;; each unwind step: 0.18ms for 200 lines, 152ms for 8000, quadrupling for
;; every doubling. It now writes into a string output port — 0.80ms at
;; 8000, and byte-identical output, which is what these pin.

(assert-equal "joining nothing gives the empty string" "" (wgsl-join '() ", "))
(assert-equal "one element is itself, with no separator" "a" (wgsl-join '("a") ", "))
(assert-equal "two elements take one separator" "a, b" (wgsl-join '("a" "b") ", "))
(assert-equal "and n elements take n-1" "a|b|c|d" (wgsl-join '("a" "b" "c" "d") "|"))
(assert-equal "an empty separator concatenates" "abc" (wgsl-join '("a" "b" "c") ""))
(assert-equal "empty elements are preserved, not skipped"
              "a||b" (wgsl-join '("a" "" "b") "|"))
(assert-equal "a separator occurring inside an element is untouched"
              "a, b, c" (wgsl-join '("a, b" "c") ", "))

;; The shape that matters: joining n elements of length L must produce
;; exactly n*L + (n-1)*|sep| characters. A fold that lost or duplicated a
;; tail would show up here as a length, without timing anything.
(assert-equal "the joined length is exactly right"
              (+ (* 500 4) (* 499 2))
              (string-length
               (wgsl-join (let loop ((i 0) (acc '()))
                            (if (= i 500) acc (loop (+ i 1) (cons "abcd" acc))))
                          ", ")))

;;--- fold-i: a bounded fold ---------------------------------------------
;; A FOLD, NOT A LOOP: an accumulator and a compile-time bound, no
;; mutation, no break, no early exit. That is what keeps the language
;; pure-expression — the whole form is one value, so it nests inside
;; arithmetic and inside itself.

(wgsl-declare! 'wall-a "wall_a" '(:u32) :vec2f)
(wgsl-declare! 'obs "obs" '(:u32) :f32)

(assert-equal "a fold is an expression of the accumulator's type"
              :f32 (wgsl-type '(fold-i 4 0.0 (k acc) (+ acc 1.0)) '()))
(assert-equal "and of a vector accumulator too"
              :vec3f (wgsl-type '(fold-i 4 (vec3 0.0 0.0 0.0) (k acc) acc) '()))

;; The index is :u32 — an address, not a quantity. WGSL has no implicit
;; coercion, so using it as a number has to say (f32 k).
(set! wgsl-counter 0)
(assert-true "the index is a u32, converted only when asked"
             (string-contains? (wgsl-body '(fold-i 4 0.0 (k acc) (f32 k)) '() "")
                               "acc_2 = f32(k_1);"))
(assert-equal "so using it as a float without saying so is refused"
              'raised (guard (e (#t 'raised))
                        (wgsl-type '(fold-i 4 0.0 (k acc) (+ acc k)) '())))
(assert-equal "and a body of the wrong type is refused"
              'raised (guard (e (#t 'raised))
                        (wgsl-type '(fold-i 4 0.0 (k acc) (vec2 1.0 2.0)) '())))
(assert-equal "a non-literal bound is refused"
              'raised (guard (e (#t 'raised)) (wgsl-type '(fold-i n 0.0 (k acc) acc) '())))
(assert-equal "as is reusing one name for both"
              'raised (guard (e (#t 'raised)) (wgsl-type '(fold-i 4 0.0 (k k) acc) '())))

;; The shape that matters: the body's own let-lifts belong INSIDE the loop.
;; Hoisting them, as every other form here does, would evaluate them once
;; against the first index and reuse the answer for every iteration.
(set! wgsl-counter 0)
(define fsrc
  (wgsl-body '(fold-i 3 0.0 (k acc) (let ((d (obs k))) (+ acc d))) '() ""))
(assert-true "the accumulator is a var, seeded before the loop"
             (string-contains? fsrc "var acc_2 : f32 = 0.0;"))
(assert-true "the loop counts to the literal bound"
             (string-contains? fsrc "for (var k_1 : u32 = 0u; k_1 < 3u; k_1 = k_1 + 1u) {"))
(assert-true "the body's bindings are INSIDE the loop, not hoisted above it"
             (string-contains? fsrc "  let d_3 : f32 = obs(k_1);"))
(assert-true "and the accumulator is rebound at the end of each pass"
             (string-contains? fsrc "  acc_2 = (acc_2 + d_3);"))

;; Nesting is the real requirement — the sensor model is a fold over rays
;; whose body is a fold over walls.
(set! wgsl-counter 0)
(define nsrc (wgsl-body '(fold-i 41 0.0 (k acc)
                           (+ acc (fold-i 12 9.0 (w best) (min best (f32 w)))))
                        '() ""))
(assert-true "an inner fold's var sits inside the outer loop"
             (string-contains? nsrc "  var best_4 : f32 = 9.0;"))
(assert-true "and so does its loop"
             (string-contains? nsrc "  for (var w_3 : u32 = 0u; w_3 < 12u;"))

;;--- let binds sequentially ---------------------------------------------
;; It lowers to a run of WGSL let statements, so each binding is in scope
;; for the next. let* is the same form, accepted because that is what a
;; Scheme programmer writes when they mean it.
(assert-equal "a later binding may use an earlier one"
              :f32 (wgsl-type '(let ((a 1.0) (b (* a 2.0))) b) '()))
(assert-equal "and let* is the same form"
              :f32 (wgsl-type '(let* ((a 1.0) (b (* a 2.0))) b) '()))

;;--- a hyphen is not a WGSL identifier character -------------------------
;; A hyphen is the ordinary way to spell a compound name in Scheme and is
;; illegal in WGSL. Emitting one produced text this checker accepted and
;; the browser's shader compiler would have rejected — the exact class of
;; failure the checker exists to move to Scheme. So every place a name
;; reaches the output underscores it: a let-bound local, a function
;; parameter both where it is declared and where the body refers to it,
;; and the function's own name.

(set! wgsl-counter 0)
(assert-true "a hyphenated local is underscored"
             (string-contains? (wgsl-body '(let ((my-var 1.0)) (* my-var 2.0)) '() "")
                               "my_var_1"))
(assert-false "and no hyphen survives into the text"
              (string-contains? (wgsl-body '(let ((my-var 1.0)) my-var) '() "")
                                "my-var"))

(define-gpu (hyphen-probe (in-x :f32) (in-y :f32)) (+ in-x in-y))
(assert-true "a hyphenated parameter is underscored where it is declared"
             (string-contains? (wgsl-definitions-source) "in_x : f32"))
(assert-true "and identically where the body refers to it"
             (string-contains? (wgsl-definitions-source) "(in_x + in_y)"))
(assert-true "as is the function's own name"
             (string-contains? (wgsl-definitions-source) "fn hyphen_probe("))

;; A hyphen is not the only offender, and the others are commoner still: a
;; WGSL identifier admits letters, digits and underscore, while `outside?`
;; and `set!` are how Scheme habitually spells a predicate and a mutator.
(set! wgsl-counter 0)
(assert-true "a question mark is underscored too"
             (string-contains?
              (wgsl-body '(let ((outside? (< time 1.0))) (if outside? 1.0 2.0)) E "")
              "outside__1"))
(set! wgsl-counter 0)
(assert-true "and a bang"
             (string-contains? (wgsl-body '(let ((fill! 1.0)) (* fill! 2.0)) '() "")
                               "fill__1"))
;; Substituted rather than dropped: `a?` and `a!` are different names, and
;; deleting the offending character would collide them onto one.
(assert-equal "an illegal character becomes an underscore, it is not deleted"
              "a_" (wgsl-underscore "a?"))

;;--- expt, because a dual body must be valid in both languages -----------
;; The kernel side has always spelled this `pow`, which is not a Scheme
;; procedure — so a define-dual written with it would type-check for the
;; device and fail as an unbound variable on its first host call.

(assert-equal "expt compiles to WGSL's pow"
              "pow(time, 2.0)" (wgsl-code '(expt time 2) E))
(assert-equal "and pow still does, for a kernel that never runs on the host"
              "pow(time, 2.0)" (wgsl-code '(pow time 2) E))


;;--- define-dual: one definition, two citizenships -----------------------
;; The form's whole claim is that the body is written ONCE, so the tests
;; are not "does each half work" but "did both halves come from the same
;; text": the Scheme procedure computes, the signature is registered with
;; a DERIVED result type, and a call site type-checks against it.
;;
;; curve-elem is the subject rather than a toy because it is the helper
;; the staged curve-fit model needs, and because its hyphen exercises the
;; name transform WGSL requires.

(define-dual (curve-elem (x :f32) (a :f32) (b :f32) (c :f32))
  (+ (* a x x) (* b x) c))

(assert-equal "the Scheme half is an ordinary procedure the VM runs"
              17.0 (curve-elem 2.0 3.0 2.0 1.0))
(assert-equal "the kernel half derived its result type rather than asserting one"
              :f32 (list-ref (wgsl-signature 'curve-elem) 3))
(assert-equal "and took the underscored name WGSL requires"
              "curve_elem" (list-ref (wgsl-signature 'curve-elem) 1))
(assert-equal "a call site type-checks against that signature"
              :f32 (wgsl-type '(curve-elem 1.0 2.0 3.0 4.0) '()))
(assert-true "and the definition reached the module"
             (string-contains? (wgsl-definitions-source) "fn curve_elem("))

;; The two ways a call site can be wrong, both caught in Scheme rather
;; than by the browser's shader log.
(assert-equal "a call with too few arguments is refused"
              'raised (guard (e (#t 'raised)) (wgsl-type '(curve-elem 1.0) '())))
(assert-equal "and one with the wrong argument type"
              'raised (guard (e (#t 'raised))
                        (wgsl-type '(curve-elem (vec2 1 1) 2.0 3.0 4.0) '())))

;; A mistake INSIDE the body is caught when the function is defined, not
;; at some later call — the property define-gpu already had and that
;; define-dual inherits by delegating to it.
(assert-equal "a type error in the body is caught at definition time"
              'raised (guard (e (#t 'raised))
                        (wgsl-define-fn! 'bad-mix '((v :vec2f) (w :vec3f)) '(+ v w))))

;;--- the screen-space derivatives, and the one stage that matters --------
;; dpdx and friends are computed by differencing across the 2x2 quad a
;; FRAGMENT shader runs in. A compute shader has no quad, so the browser
;; rejects them there — the one failure this checker must catch, since a
;; shader log in a console is what it exists to avoid.

(assert-equal "dpdx keeps its argument's type, in a fragment compile"
              :vec3f (with-wgsl-stage :fragment (wgsl-type '(dpdx col) E)))
(assert-equal "fwidth too" :f32
              (with-wgsl-stage :fragment (wgsl-type '(fwidth time) E)))
(assert-equal "and it is refused in a compute compile"
              'raised (guard (e (#t 'raised))
                        (with-wgsl-stage :compute (wgsl-type '(dpdx time) E))))
;; "Not saying" must not count as permission: an unstaged compile is
;; exactly the case where nothing knows where the code will run.
(assert-equal "and refused when nothing said what the stage is"
              'raised (guard (e (#t 'raised)) (wgsl-type '(dpdx time) E)))
;; The stage goes back even when the compile raises, or a later unstaged
;; compile would inherit permission it never asked for.
(assert-equal "the stage is restored after a raise inside it"
              'raised
              (begin (guard (e (#t 'ignored))
                       (with-wgsl-stage :fragment (wgsl-type '(+ uv col) E)))
                     (guard (e (#t 'raised)) (wgsl-type '(dpdx time) E))))

;; The reason they were added: normalising an implicit function by its
;; screen-space gradient gives distance in PIXELS, so a stroke is a
;; uniform width without anyone converting plot units by hand.
(assert-equal "the near-SDF idiom type-checks to a scalar"
              :f32
              (with-wgsl-stage :fragment
                (wgsl-type '(let* ((g (- (* time time) (swizzle uv y)))
                                   (d (/ (abs g)
                                         (length (vec2 (dpdx g) (dpdy g))))))
                              (smoothstep 0.0 1.5 d))
                           E)))

;;--- a literal that cannot survive shader creation ----------------------
;; WGSL evaluates a const-expression at shader-creation time, so a literal
;; argument makes one, and `log(0.0)` is not -inf — it is the error "value
;; -Infinity cannot be represented as '<AbstractFloat>'", reported by the
;; browser with no line number in our source.
;;
;; This is not hypothetical. logpdf-exponential's dual body said
;; (log 0.0), which is correct Scheme, and put a shader that would not
;; compile into every assembled module.

(assert-equal "log of a literal zero is refused"
              'raised (guard (e (#t 'raised)) (wgsl-type '(log 0.0) '())))
(assert-equal "as is sqrt of a negative literal"
              'raised (guard (e (#t 'raised)) (wgsl-type '(sqrt -1.0) '())))
(assert-equal "and an exp that overflows"
              'raised (guard (e (#t 'raised)) (wgsl-type '(exp 1000) '())))
(assert-equal "and an acos outside its domain"
              'raised (guard (e (#t 'raised)) (wgsl-type '(acos 2.0) '())))

;; The refusal must not overreach. A finite literal is fine, and a RUNTIME
;; argument is never a const-expression however impossible its value —
;; log(scale) is exactly what logpdf_normal does.
(assert-equal "a finite literal is untouched" :f32 (wgsl-type '(log 2.0) '()))
(assert-equal "and a runtime argument is not a const-expression at all"
              :f32 (wgsl-type '(log time) E))
(assert-equal "nor is a computed one" :f32 (wgsl-type '(log (* 0.0 time)) E))

;;--- a non-finite literal becomes a call --------------------------------
;; The reader already reads -inf, inf and nan as numbers, so a kernel
;; spells them the way Scheme does. WGSL has no spelling for any of them
;; and will not compute one at shader-creation time, so this is where they
;; stop being literals: each becomes a helper that divides by a runtime
;; zero, which is the only way to reach the IEEE value.
;;
;; A model that wants to say "this configuration is impossible" — the
;; rocket exploded, score it -inf — therefore needs no vocabulary it would
;; not otherwise have.

(assert-equal "negative infinity emits a runtime helper"
              "neg_inf()" (wgsl-code '-inf '()))
(assert-equal "positive infinity too" "pos_inf()" (wgsl-code 'inf '()))
(assert-equal "and NaN"               "nan_f32()" (wgsl-code 'nan '()))
(assert-equal "they are still scalars" :f32 (wgsl-type '-inf '()))
(assert-equal "and compose like any other value"
              "(time + neg_inf())" (wgsl-code '(+ time -inf) E))
(assert-equal "an ordinary literal is untouched" "2.5" (wgsl-code '2.5 '()))

;; The helpers must be DEFINED wherever they are called, and before it —
;; WGSL has no forward declarations. They are registered at load so they
;; sit first in emission order.
(assert-true "the helpers are in the definitions"
             (string-contains? (wgsl-definitions-source) "fn neg_inf()"))
(assert-true "over a runtime var, which is the whole trick"
             (string-contains? (wgsl-definitions-source) "var z : f32 = 0.0;"))

;;--- loading the compiler twice must not forget anything -----------------
;; Every table here is module state, and a plain (define t '()) re-runs on
;; a second load and resets it. Transitive double-loading is the normal
;; case once more than one library wants the kernel compiler — lib/dist.scm
;; and lib/wrangle.scm both do — so this silently discarded every
;; signature and definition registered by whichever loaded first.

(assert-true "a signature registered before a reload survives it"
             (begin (load "lib/wgsl.scm")
                    (if (wgsl-signature 'curve-elem) #t #f)))
(assert-true "and so does its definition"
             (string-contains? (wgsl-definitions-source) "fn curve_elem("))

;;--- put the stub declarations back ------------------------------------
;; draw, wall-a and obs were declared above to exercise call sites; no
;; WGSL of those names exists anywhere. Layer 18 checks that every
;; declared signature has a definition in the assembled shader — a real
;; test, which once caught an `unresolved call target` that only the
;; browser could see — so leaving these behind would fail it with a
;; promise this file made and cannot keep.
;;
;; They used to be cleared by accident, because loading lib/wgsl.scm a
;; second time reset the table. It no longer does.
(wgsl-forget-declaration! 'draw)
(wgsl-forget-declaration! 'wall-a)
(wgsl-forget-declaration! 'obs)

(suite-summary)
