;;----------------------------------------------------------------------
;; Instanced points in 3D: a buffer of point data, drawn as sprites
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
;; LAYOUT. Seven floats per point, flat:
;;
;;   0: x     world space
;;   1: y
;;   2: z
;;   3: size  radius, in world units at the camera's distance
;;   4: r
;;   5: g
;;   6: b
;;
;; A flat array<f32>, NOT an array of structs, and that is not laziness.
;; WGSL gives vec3<f32> a 16-byte alignment inside a storage array, so
;; { pos : vec3<f32>, size : f32, colour : vec3<f32> } does not pack the
;; way those same fields pack tightly on the host. The mismatch is silent:
;; no error, just points drawn in the wrong places with the wrong colours.
;; Indexing a flat float array by hand sidesteps the alignment rules
;; completely. The stride appears here and in the shader below, and layer
;; 17 asserts the shader against points-stride so the two cannot drift.
;;
;; CAMERA. Parameters, not a matrix: yaw, pitch, distance and fov travel in
;; the uniform and the vertex shader does the rotate-and-project itself.
;; That is ten lines of WGSL against a mat4 library in Scheme that nothing
;; else yet wants. If arbitrary cameras ever turn up, that is the moment to
;; add a 16-float path — not before.
;;
;; NO DEPTH BUFFER, on purpose. Additive blending is commutative, so a
;; glowing point cloud is order-independent and the usual reason to sort or
;; depth-test simply does not arise. Perspective shrinks distant points,
;; which reads as depth on its own.
;;----------------------------------------------------------------------

(define points-stride 7)

;; (make-points n) -> bytes, sized for n points and sealed.
;; Sealed rather than Building: a buffer you bind is fixed-size, which is
;; exactly the distinction ObjBytes::Residency draws.
(define (make-points n)
  (let ((b (make-bytes (* n points-stride 4))))
    (bytes-seal! b)
    b))

;; An f32 view over the whole buffer. Indices are FLOAT indices, so point i
;; occupies [i*7, i*7+7).
(define (points-view b) (bytes-view b :f32))

(define (point-set! v i x y z size r g b)
  (let ((k (* i points-stride)))
    (view-set! v (+ k 0) x)
    (view-set! v (+ k 1) y)
    (view-set! v (+ k 2) z)
    (view-set! v (+ k 3) size)
    (view-set! v (+ k 4) r)
    (view-set! v (+ k 5) g)
    (view-set! v (+ k 6) b)))

(define (point-x v i) (view-ref v (* i points-stride)))
(define (point-y v i) (view-ref v (+ 1 (* i points-stride))))
(define (point-z v i) (view-ref v (+ 2 (* i points-stride))))

;;--- the camera ---------------------------------------------------------
;; A plain mutable 4-vector, so a demo can orbit it from inside update!
;; without another callback or any hidden state in the host.

(define (make-camera) (vector 0.6 0.35 3.2 1.6))
(define (camera-yaw c)      (vector-ref c 0))
(define (camera-pitch c)    (vector-ref c 1))
(define (camera-distance c) (vector-ref c 2))
(define (camera-fov c)      (vector-ref c 3))
(define (camera-yaw-set! c v)      (vector-set! c 0 v))
(define (camera-pitch-set! c v)    (vector-set! c 1 v))
(define (camera-distance-set! c v) (vector-set! c 2 v))

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
   "  yaw : f32,\n"
   "  pitch : f32,\n"
   "  dist : f32,\n"
   "  fov : f32,\n"
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
   "  let base = ii * 7u;\n"
   "  let px = pts[base + 0u];\n"
   "  let py = pts[base + 1u];\n"
   "  let pz = pts[base + 2u];\n"
   "  let ps = pts[base + 3u];\n"
   "\n"
   "  // Yaw about Y, then pitch about X, then push the world away from\n"
   "  // the eye by dist. Written out rather than assembled from matrices\n"
   "  // because it is only two rotations and this way there is nothing to\n"
   "  // keep in sync with a host-side matrix library.\n"
   "  let cy = cos(u.yaw);\n"
   "  let sy = sin(u.yaw);\n"
   "  let rx = px * cy + pz * sy;\n"
   "  let rz = pz * cy - px * sy;\n"
   "  let cp = cos(u.pitch);\n"
   "  let sp = sin(u.pitch);\n"
   "  let ry = py * cp - rz * sp;\n"
   "  let rzz = rz * cp + py * sp;\n"
   "  let zc = rzz + u.dist;\n"
   "\n"
   "  // Perspective divide. zc is clamped away from zero so a point at or\n"
   "  // behind the eye cannot produce an infinity; such points are pushed\n"
   "  // outside clip space below rather than smeared across the screen.\n"
   "  let f = u.fov / max(zc, 0.05);\n"
   "  let aspect = u.width / u.height;\n"
   "  let c = corners[vi];\n"
   "  let cx = (rx * f) / aspect + c.x * ps * f / aspect;\n"
   "  let cyy = ry * f + c.y * ps * f;\n"
   "\n"
   "  var o : VSOut;\n"
   "  o.pos = select(vec4<f32>(9.0, 9.0, 0.0, 1.0),\n"
   "                 vec4<f32>(cx, cyy, 0.0, 1.0),\n"
   "                 zc > 0.05);\n"
   "  o.quad = c;\n"
   "  o.tint = vec3<f32>(pts[base + 4u], pts[base + 5u], pts[base + 6u]);\n"
   "  return o;\n"
   "}\n"
   "\n"
   "@fragment\n"
   "fn fs(v : VSOut) -> @location(0) vec4<f32> {\n"
   "  let d = abs(v.quad.x) + abs(v.quad.y); ///Users/colin/Library/Application Support/CleanShot/media/media_e7ctMJUOE6/CleanShot 2026-08-21 at 15.01.57@2x.png  length(v.quad);\n"
   "  //if (d > 1.0) { discard; }\n"
   "  let a = 1.0 - smoothstep(0.35, 1.0, d);\n"
   "  return vec4<f32>(v.tint, a);\n"
   "}\n"))
