;;----------------------------------------------------------------------
;; Instanced points: a buffer of point data, drawn as sprites
;;
;; The second harness. lib/shadertoy.scm draws one full-screen quad and
;; computes a colour per pixel; this one draws one small quad per POINT and
;; reads its attributes from a storage buffer. Same GPU, opposite shape:
;; there the work is per-pixel and stateless, here it is per-element and
;; the elements persist between frames.
;;
;; Who fills the buffer is deliberately not this file's business. Scheme
;; fills it today — a fiber per actor will fill it next — and a compute
;; wrangle will fill it after that, without any of them changing what is
;; below.
;;
;; LAYOUT. Six floats per point, flat:
;;
;;   0: x    clip space, -1 .. 1
;;   1: y    clip space, -1 .. 1
;;   2: size radius in clip units
;;   3: r
;;   4: g
;;   5: b
;;
;; A flat array<f32>, NOT an array of structs, and that is not laziness.
;; WGSL gives vec3<f32> a 16-byte alignment inside a storage array, so
;; { pos : vec2<f32>, size : f32, colour : vec3<f32> } does not pack the
;; way those same fields pack tightly on the host. The mismatch is silent:
;; no error, just points drawn in the wrong places with the wrong colours.
;; Indexing a flat float array by hand sidesteps the alignment rules
;; completely. The stride appears here and in js_gpu_draw_instances, and
;; nowhere else.
;;----------------------------------------------------------------------

(define points-stride 6)

;; (make-points n) -> bytes, sized for n points and sealed.
;; Sealed rather than Building: a buffer you bind is fixed-size, which is
;; exactly the distinction ObjBytes::Residency draws.
(define (make-points n)
  (let ((b (make-bytes (* n points-stride 4))))
    (bytes-seal! b)
    b))

;; An f32 view over the whole buffer. Indices are FLOAT indices, so point i
;; occupies [i*6, i*6+6).
(define (points-view b) (bytes-view b :f32))

(define (point-set! v i x y size r g b)
  (let ((k (* i points-stride)))
    (view-set! v (+ k 0) x)
    (view-set! v (+ k 1) y)
    (view-set! v (+ k 2) size)
    (view-set! v (+ k 3) r)
    (view-set! v (+ k 4) g)
    (view-set! v (+ k 5) b)))

(define (point-x v i) (view-ref v (* i points-stride)))
(define (point-y v i) (view-ref v (+ 1 (* i points-stride))))

;; The shader. Hand-written rather than emitted by lib/wgsl.scm: it is
;; plumbing, not a kernel — it has statements, a discard, and builtins the
;; kernel language does not have. The kernel compiler will earn its place
;; here when a per-point wrangle computes the attributes; drawing them is
;; not that job.
(define points-wgsl
  (string-append
   "struct U {\n"
   "  time : f32,\n"
   "  width : f32,\n"
   "  height : f32,\n"
   "  count : f32,\n"
   "};\n"
   "@group(0) @binding(0) var<uniform> u : U;\n"
   "@group(0) @binding(1) var<storage, read> pts : array<f32>;\n"
   "\n"
   "struct VSOut {\n"
   "  @builtin(position) pos : vec4<f32>,\n"
   "  @location(0) quad : vec2<f32>,\n"
   "  @location(1) tint : vec3<f32>,\n"
   "};\n"
   "\n"
   "@vertex\n"
   "fn vs(@builtin(vertex_index) vi : u32,\n"
   "      @builtin(instance_index) ii : u32) -> VSOut {\n"
   "  var corners = array<vec2<f32>, 6>(\n"
   "    vec2<f32>(-1.0, -1.0), vec2<f32>( 1.0, -1.0), vec2<f32>(-1.0,  1.0),\n"
   "    vec2<f32>(-1.0,  1.0), vec2<f32>( 1.0, -1.0), vec2<f32>( 1.0,  1.0)\n"
   "  );\n"
   "  let base = ii * 6u;\n"
   "  let px = pts[base + 0u];\n"
   "  let py = pts[base + 1u];\n"
   "  let ps = pts[base + 2u];\n"
   "  let c = corners[vi];\n"
   "  let aspect = u.width / u.height;\n"
   "  var o : VSOut;\n"
   "  o.pos = vec4<f32>(px + c.x * ps / aspect, py + c.y * ps, 0.0, 1.0);\n"
   "  o.quad = c;\n"
   "  o.tint = vec3<f32>(pts[base + 3u], pts[base + 4u], pts[base + 5u]);\n"
   "  return o;\n"
   "}\n"
   "\n"
   "@fragment\n"
   "fn fs(v : VSOut) -> @location(0) vec4<f32> {\n"
   "  let d = length(v.quad);\n"
   "  if (d > 1.0) { discard; }\n"
   "  let a = 1.0 - smoothstep(0.35, 1.0, d);\n"
   "  return vec4<f32>(v.tint, a);\n"
   "}\n"))
