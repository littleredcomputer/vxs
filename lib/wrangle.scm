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
;;
;; rng_init is called for you, addressed by point index and w.seed, so
;; draws are reproducible per point and independent between points.
;;----------------------------------------------------------------------

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
        (stat (embedded-source "stat.wgsl")))
    (if (not (and rng stat))
        (error 'wrangle "rng.wgsl or stat.wgsl is missing from the binary"))
    (string-append rng "\n" stat "\n" wrangle-preamble body "\n}\n")))
