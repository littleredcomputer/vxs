;;----------------------------------------------------------------------
;; Layer 16: the Shadertoy harness (lib/shadertoy.scm)
;;
;; lib/wgsl.scm compiles a kernel and knows nothing about where it runs.
;; This layer covers the smallest HARNESS for one: a fragment shader over a
;; fullscreen triangle. A point-buffer wrangle will be a sibling harness
;; around the same compiler, so what is tested here is the wrapping — that
;; the entry points exist, that the kernel lands inside a function with the
;; right signature, and that a kernel of the wrong TYPE is refused before
;; it can reach a GPU.
;;
;; The coupling this file exists to protect: struct U's field order is
;; also written by hand on the host side, in js_gpu_run_kernel's
;; Float32Array([time, width, height, 0]). Those two orders have to agree
;; and nothing but a test says so — get it wrong and the shader animates
;; on canvas width, which looks like a frozen image rather than an error.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")
(load "lib/shadertoy.scm")

(test-suite "16_shadertoy: fullscreen fragment harness")

;; Substring search, since the assertions below are about structure rather
;; than an exact rendering of the whole shader — pinning the entire text
;; would make every cosmetic change to the preamble a test failure.

(define plasma
  '(let ((c (- uv 0.5))
         (r (length c)))
     (vec3 (sin (- (* r 24.0) time)) 0.2 0.8)))

(define src (shadertoy plasma))

;;--- the harness is present ---------------------------------------------

(assert-true "emits a vertex entry point"   (string-contains? src "@vertex"))
(assert-true "emits a fragment entry point" (string-contains? src "@fragment"))
(assert-true "vertex entry is named vs"     (string-contains? src "fn vs("))
(assert-true "fragment entry is named fs"   (string-contains? src "fn fs("))
(assert-true "declares the uniform binding"
             (string-contains? src "@group(0) @binding(0) var<uniform> u : U;"))

;; Field order is a contract with the host's Float32Array write.
(assert-true "uniform struct declares time first"
             (string-contains? src "struct U {\n  time : f32,\n  width : f32,\n  height : f32,"))

;; A fullscreen triangle, not a quad: no vertex buffer and no diagonal seam.
(assert-true "draws from a 3-vertex array"
             (string-contains? src "array<vec2<f32>, 3>"))
(assert-true "positions come from vertex_index"
             (string-contains? src "@builtin(vertex_index)"))

;; `in` and `out` are reserved words in WGSL; using either as the varying
;; name compiles to a syntax error that has nothing to do with the kernel.
(assert-false "does not use `in` as an identifier"
              (string-contains? src "fn fs(in :"))
(assert-false "does not use `out` as an identifier"
              (string-contains? src "var out :"))

;;--- the kernel lands inside it -----------------------------------------

(assert-true "kernel has the expected signature"
             (string-contains?
              src "fn kernel(uv : vec2<f32>, time : f32, res : vec2<f32>) -> vec3<f32> {"))
(assert-true "the compiled body is inside the kernel"
             (string-contains? src "let r_2 : f32 = length(c_1);"))
(assert-true "the fragment stage calls the kernel with the uniforms"
             (string-contains? src "kernel(v.uv, u.time, vec2<f32>(u.width, u.height))"))

;;--- rejection ----------------------------------------------------------

(define (rejects? body)
  (guard (e (#t #t)) (shadertoy body) #f))

;; A kernel returning the wrong width is the mistake worth catching here:
;; it is well-typed as an expression and only wrong as a KERNEL, so
;; lib/wgsl.scm alone would happily compile it.
(assert-true "a vec2 kernel is rejected"  (rejects? '(vec2 1 2)))
(assert-true "a vec4 kernel is rejected"  (rejects? '(vec4 1 2 3 4)))
(assert-true "a scalar kernel is rejected" (rejects? 'time))
(assert-true "a type error inside the kernel still propagates"
             (rejects? '(vec3 (+ uv time) 0 0)))

(assert-true "a vec3 kernel is accepted"
             (string? (shadertoy '(vec3 (swizzle uv x) 0 0))))
;; Dotted component access is Shadertoy/GLSL habit, not this language.
;; It reads as one symbol, so it fails as an unbound variable rather than
;; as a syntax error, which is worth having a test say out loud.
(assert-true "uv.x is not swizzle syntax here" (rejects? '(vec3 uv.x 0 0)))

;;--- what the kernel may refer to ---------------------------------------

(assert-equal "uv is a vec2" :vec2f (wgsl-type 'uv shadertoy-env))
(assert-equal "time is a scalar" :f32 (wgsl-type 'time shadertoy-env))
(assert-equal "res is a vec2" :vec2f (wgsl-type 'res shadertoy-env))
(assert-true "an unknown name is rejected" (rejects? '(vec3 mouse 0 0)))

;;--- this harness is the one with a quad --------------------------------
;; The screen-space derivatives exist only where there are neighbouring
;; fragments to difference against, so `shadertoy` compiles as :fragment
;; and admits them. The assertion is that the HARNESS says so — the
;; mechanism itself is layer 15's.

;;--- and it emits the definitions a kernel may call ----------------------
;; It used not to. A kernel could call a define-gpu or define-dual
;; function, type-check here, and then fail in the browser as an
;; unresolved call target — the one failure this checker exists to catch
;; rather than forward. lib/wrangle.scm had always emitted them; this
;; harness had not, and nothing noticed because no fragment kernel had
;; wanted one yet. The curve-fit plot wants one immediately.

(define-gpu (shadertoy-probe (x :f32)) (* x 2.0))

(assert-true "a define-gpu function reaches the shader"
             (string-contains? (shadertoy '(vec3 (shadertoy-probe time) 0 0))
                               "fn shadertoy_probe("))
(assert-true "and the non-finite helpers do too, since a literal calls one"
             (string-contains? (shadertoy '(vec3 -inf 0 0)) "fn neg_inf("))

;; Declared BEFORE the kernel, because WGSL has no forward declarations.
;; A definition that arrived after its use would compile here and fail
;; there, which is the same failure wearing a different hat.
(define (index-of hay needle)
  (let ((h (string-length hay)) (n (string-length needle)))
    (let loop ((k 0))
      (cond ((> (+ k n) h) #f)
            ((string=? (substring hay k (+ k n)) needle) k)
            (else (loop (+ k 1)))))))

(assert-true "definitions precede the kernel that calls them"
             (let* ((src (shadertoy '(vec3 (shadertoy-probe time) 0 0)))
                    (def (index-of src "fn shadertoy_probe("))
                    (use (index-of src "fn kernel(")))
               (and def use (< def use))))

(assert-true "a kernel may take a screen-space derivative here"
             (string-contains? (shadertoy '(vec3 (fwidth time) 0 0)) "fwidth("))
(assert-true "and the near-SDF idiom compiles, distance in pixels"
             (string-contains?
              (shadertoy '(let* ((g (- (swizzle uv y) 0.5))
                                 (d (/ (abs g)
                                       (length (vec2 (dpdx g) (dpdy g))))))
                            (vec3 (smoothstep 0.0 1.5 d) 0 0)))
              "dpdy("))

(suite-summary)
