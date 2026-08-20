;;----------------------------------------------------------------------
;; The compute wrangle: a kernel that rewrites the point buffer in place
;;
;; Third harness. shadertoy is pixel -> colour, points is a draw, and this
;; is point -> point: the same buffer the renderer reads, rewritten on the
;; GPU instead of by Scheme.
;;
;; DELIBERATELY MINIMAL. The attribute vocabulary here is the smallest
;; thing that makes a compute kernel work — the point index, the time, and
;; get/set for position, size and colour. It is NOT the wrangle algebra:
;; there is no @P, no attribute binding, no notion of a node. Those are the
;; interesting design questions and they should be answered from several
;; real programs, not invented under the pressure of getting one to run.
;;
;; The body is WGSL for now, not the kernel language of lib/wgsl.scm.
;; Wiring the compiler in here is exactly the step that would force the
;; vocabulary decision, so it waits.
;;
;; What the body has in scope:
;;   i     u32, the point index (already bounds-checked)
;;   w     the uniform: w.time, w.count, w.seed
;;   pt_pos(i), pt_size(i), pt_colour(i)   read
;;   pt_write(i, pos, size, colour)        write
;;   the whole of lib/rng.wgsl and lib/stat.wgsl — random_normal,
;;   random_uniform, random_gamma, flip, and the logpdf_* family
;;   heat_colour(t), cool_colour(t) from lib/colour.wgsl — display ramps,
;;   not attributes
;;
;; rng_init is called for you, addressed by point index and w.seed, so
;; draws are reproducible per point and independent between points.
;;----------------------------------------------------------------------

;; The kernel compiler, for wgsl-declare! and define-gpu. Explicit rather
;; than relying on lib/gpu.scm having loaded it first: this file is loaded
;; directly by layer 18 and by anything that wants only the wrangle.
(load "lib/wgsl.scm")

;;--- what a Scheme kernel can call --------------------------------------
;; Signatures for the hand-written WGSL in lib/stat.wgsl and
;; lib/colour.wgsl. They are asserted rather than derived because nothing
;; can read a signature out of WGSL text — which is exactly the difference
;; between these and a define-gpu function, whose result type the compiler
;; works out for itself.
;;
;; Several of these could NOT be written in the kernel language: random_gamma
;; needs a rejection loop and erfc needs a polynomial evaluated in sequence.
;; That is the point of having two tiers rather than one. When the language
;; grows a curated `for-i`, some of them could move across, and no caller
;; would change.

(wgsl-declare! 'random-uniform     "random_uniform"     '(f32 f32)     'f32)
(wgsl-declare! 'random-normal      "random_normal"      '(f32 f32)     'f32)
(wgsl-declare! 'random-exponential "random_exponential" '(f32)         'f32)
(wgsl-declare! 'random-gamma       "random_gamma"       '(f32 f32)     'f32)
(wgsl-declare! 'logpdf-normal      "logpdf_normal"      '(f32 f32 f32) 'f32)
(wgsl-declare! 'logpdf-uniform     "logpdf_uniform"     '(f32 f32 f32) 'f32)
(wgsl-declare! 'erfc               "erfc"               '(f32)         'f32)
(wgsl-declare! 'inv-erf            "inv_erf"            '(f32)         'f32)
(wgsl-declare! 'heat-colour        "heat_colour"        '(f32)         'vec3f)
(wgsl-declare! 'cool-colour        "cool_colour"        '(f32)         'vec3f)

;; What a wrangle kernel written in Scheme sees. `time` and `seed` are
;; struct fields on the uniform, which the environment handles directly:
;; a binding may carry the name to EMIT alongside the type, so the Scheme
;; name and the WGSL spelling need not match.
(define wrangle-env
  '((time . (f32 . "w.time"))
    (seed . (f32 . "w.seed"))
    (count . (f32 . "w.count"))))

(define wrangle-stride 7)   ; must match points-stride in lib/points.scm

(define wrangle-preamble
  (string-append
   "\n"
   "struct WU {\n"
   "  time : f32,\n"
   "  count : f32,\n"
   "  seed : f32,\n"
   "  pad : f32,\n"
   "};\n"
   "@group(0) @binding(0) var<uniform> w : WU;\n"
   "@group(0) @binding(1) var<storage, read_write> pts : array<f32>;\n"
   "\n"
   "fn pt_pos(i : u32) -> vec3<f32> {\n"
   "  let b = i * 7u;\n"
   "  return vec3<f32>(pts[b + 0u], pts[b + 1u], pts[b + 2u]);\n"
   "}\n"
   "fn pt_size(i : u32) -> f32 { return pts[i * 7u + 3u]; }\n"
   "fn pt_colour(i : u32) -> vec3<f32> {\n"
   "  let b = i * 7u;\n"
   "  return vec3<f32>(pts[b + 4u], pts[b + 5u], pts[b + 6u]);\n"
   "}\n"
   "fn pt_write(i : u32, p : vec3<f32>, size : f32, col : vec3<f32>) {\n"
   "  let b = i * 7u;\n"
   "  pts[b + 0u] = p.x;\n"
   "  pts[b + 1u] = p.y;\n"
   "  pts[b + 2u] = p.z;\n"
   "  pts[b + 3u] = size;\n"
   "  pts[b + 4u] = col.x;\n"
   "  pts[b + 5u] = col.y;\n"
   "  pts[b + 6u] = col.z;\n"
   "}\n"
   "\n"
   "@compute @workgroup_size(64)\n"
   "fn main(@builtin(global_invocation_id) gid : vec3<u32>) {\n"
   "  let i = gid.x;\n"
   "  // The dispatch rounds up to whole workgroups, so the tail runs with\n"
   "  // indices past the end. They must return, not clamp: clamping would\n"
   "  // have several invocations write the same point.\n"
   "  if (i >= u32(w.count)) { return; }\n"
   "  rng_init(i, u32(w.seed), 0u);\n"))

;; (wrangle-wgsl body) -> complete compute shader source.
;; `body` is WGSL statements; see the header for what is in scope.
(define (wrangle-wgsl body)
  (let ((rng (embedded-source "rng.wgsl"))
        (stat (embedded-source "stat.wgsl"))
        (col (embedded-source "colour.wgsl")))
    (if (not (and rng stat col))
        (error 'wrangle
               "rng.wgsl, stat.wgsl or colour.wgsl is missing from the binary"))
    ;; Order matters: the libraries first, then anything define-gpu has
    ;; emitted (which may call them), then the kernel. WGSL wants a
    ;; function to appear before the code that calls it, which is also the
    ;; order a reader expects.
    (string-append rng "\n" stat "\n" col "\n"
                   (wgsl-definitions-source) "\n"
                   wrangle-preamble body "\n}\n")))
