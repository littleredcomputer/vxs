;;----------------------------------------------------------------------
;; map cube: one cube per point, instead of one sprite
;;
;; The same point buffer, the same seven floats, a different primitive.
;; draw(36, N) rather than draw(6, N): twelve triangles instead of two,
;; and the vertex shader answers the same question 36 times per point —
;; where does vertex vi of instance ii go.
;;
;; THREE THINGS CHANGE, and none of them is the vertex count.
;;
;; 1. The offset is added in WORLD space, before the camera transform,
;;    rather than in clip space after it. That is the entire difference
;;    between a cube and a billboard: a sprite is offset after projection
;;    so it always faces you, a cube is offset before so it turns with the
;;    scene. It is also why a cube wants an orientation attribute and a
;;    sprite does not — see the note at the bottom.
;;
;; 2. A real w. The points shader divides by hand and emits w = 1, which
;;    is fine for something flat and camera-facing. A cube has extent along
;;    the view direction, so it needs depth in the depth buffer and
;;    perspective-correct interpolation, and both are derived from w.
;;
;; 3. Depth testing instead of additive blending. Sprites accumulate and
;;    the order does not matter, which is why they never needed a depth
;;    buffer. Cubes occlude, and occlusion is emphatically order-dependent.
;;
;; A released pool slot is still invisible for free: size 0 collapses all
;; 36 vertices onto the centre, so every triangle has zero area.
;;
;; NOT YET: per-cube orientation. Every cube here is axis-aligned in world
;; space, so they all present the same faces. Turning them individually
;; needs an orientation attribute — the first thing a sprite genuinely does
;; not need, and the reason VEX has @orient and @up at all.
;;----------------------------------------------------------------------

(load "lib/points.scm")
;; For scratch-attrs: the renderer recognises a declared `pose` attribute
;; and reads it from the same buffer the kernel writes.
(load "lib/wrangle.scm")

;; Vertices per instance. The draw call and this constant must agree, and
;; layer 21 asserts the shader against it.
(define cube-vertex-count 36)

;; Is there a stock `pose` to honour? An attribute named pose, of type
;; :quat, is the convention — declare one and the cubes turn.
(define (cubes-posed?)
  (let ((a (assq 'pose scratch-attrs)))
    (and a (eq? (cadr a) :quat))))

;; A PROCEDURE, not a constant, because the shader depends on whether a
;; pose was declared. Emitting the binding unconditionally would give every
;; cube program a storage buffer it never reads, and a different pipeline
;; layout to go with it.
(define (cubes-wgsl)
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
   ;; The SAME buffer the compute pass writes, bound here read-only. No
   ;; second copy, no upload, no synchronisation to get wrong — the kernel
   ;; sets a pose and the renderer reads it out of the slot it was put in.
   (if (cubes-posed?)
       (string-append
        "@group(0) @binding(2) var<storage, read> scratch : array<f32>;\n"
        (scratch-accessors 'pose :quat (scratch-offset 'pose) #t)
        (embedded-source "quat.wgsl") "\n")
       "")
   "\n"
   "struct VSOut {\n"
   "  @builtin(position) pos : vec4<f32>,\n"
   "  @location(0) tint : vec3<f32>,\n"
   "};\n"
   "\n"
   "@vertex\n"
   "fn vs(@builtin(vertex_index) vi : u32,\n"
   "      @builtin(instance_index) ii : u32) -> VSOut {\n"
   "  var verts = array<vec3<f32>, 36>(\n"
   "    vec3<f32>(1.0, -1.0, -1.0), vec3<f32>(1.0, 1.0, -1.0), vec3<f32>(1.0, 1.0, 1.0),\n"
   "    vec3<f32>(1.0, -1.0, -1.0), vec3<f32>(1.0, 1.0, 1.0), vec3<f32>(1.0, -1.0, 1.0),\n"
   "    vec3<f32>(-1.0, -1.0, 1.0), vec3<f32>(-1.0, 1.0, 1.0), vec3<f32>(-1.0, 1.0, -1.0),\n"
   "    vec3<f32>(-1.0, -1.0, 1.0), vec3<f32>(-1.0, 1.0, -1.0), vec3<f32>(-1.0, -1.0, -1.0),\n"
   "    vec3<f32>(-1.0, 1.0, -1.0), vec3<f32>(-1.0, 1.0, 1.0), vec3<f32>(1.0, 1.0, 1.0),\n"
   "    vec3<f32>(-1.0, 1.0, -1.0), vec3<f32>(1.0, 1.0, 1.0), vec3<f32>(1.0, 1.0, -1.0),\n"
   "    vec3<f32>(-1.0, -1.0, 1.0), vec3<f32>(-1.0, -1.0, -1.0), vec3<f32>(1.0, -1.0, -1.0),\n"
   "    vec3<f32>(-1.0, -1.0, 1.0), vec3<f32>(1.0, -1.0, -1.0), vec3<f32>(1.0, -1.0, 1.0),\n"
   "    vec3<f32>(-1.0, -1.0, 1.0), vec3<f32>(1.0, -1.0, 1.0), vec3<f32>(1.0, 1.0, 1.0),\n"
   "    vec3<f32>(-1.0, -1.0, 1.0), vec3<f32>(1.0, 1.0, 1.0), vec3<f32>(-1.0, 1.0, 1.0),\n"
   "    vec3<f32>(1.0, -1.0, -1.0), vec3<f32>(-1.0, -1.0, -1.0), vec3<f32>(-1.0, 1.0, -1.0),\n"
   "    vec3<f32>(1.0, -1.0, -1.0), vec3<f32>(-1.0, 1.0, -1.0), vec3<f32>(1.0, 1.0, -1.0)\n"
   "  );\n"
   "  var norms = array<vec3<f32>, 6>(\n"
   "    vec3<f32>(1.0, 0.0, 0.0), vec3<f32>(-1.0, 0.0, 0.0), vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, -1.0, 0.0), vec3<f32>(0.0, 0.0, 1.0), vec3<f32>(0.0, 0.0, -1.0)\n"
   "  );\n"
   "\n"
   "  let base = ii * " points-stride-wgsl ";\n"
   "  let centre = vec3<f32>(pts[base + 0u], pts[base + 1u], pts[base + 2u]);\n"
   "  let half = pts[base + 3u];\n"
   "\n"
   "  // WORLD space, and this one line is the whole difference from a\n"
   "  // billboard. A sprite adds its corner offset AFTER projection so it\n"
   "  // always faces the camera; a cube adds it here, before, so it rotates\n"
   "  // with the scene and can show you a different face as you orbit. That\n"
   "  // is also why a cube needs an orientation and a sprite does not.\n"
   ;; Rotate the corner about the cube's own centre, then place it. The
   ;; other order would swing the cube around the origin instead.
   (if (cubes-posed?)
       "  let world = centre + q_rot(attr_pose(ii), verts[vi] * half);\n"
       "  let world = centre + verts[vi] * half;\n")
   "\n"
   "  let cy = cos(u.yaw);\n"
   "  let sy = sin(u.yaw);\n"
   "  let rx = world.x * cy + world.z * sy;\n"
   "  let rz = world.z * cy - world.x * sy;\n"
   "  let cp = cos(u.pitch);\n"
   "  let sp = sin(u.pitch);\n"
   "  let ry = world.y * cp - rz * sp;\n"
   "  let rzz = rz * cp + world.y * sp;\n"
   "  let zc = rzz + u.dist;\n"
   "\n"
   "  // Flat shading from a fixed world-space light. Computed here rather than\n"
   "  // in the fragment stage because all three vertices of a face share a\n"
   "  // normal, so the interpolated result would be constant anyway. Without\n"
   "  // any shading a cube is a flat silhouette and reads as a hexagon.\n"
   ;; The NORMAL turns too. Rotating the geometry and not the normal
   ;; leaves the shading fixed to the world while the faces move under it,
   ;; which reads as a lighting bug rather than as a pose.
   (if (cubes-posed?)
       "  let n = q_rot(attr_pose(ii), norms[vi / 6u]);\n"
       "  let n = norms[vi / 6u];\n")
   ;; KEY, FILL AND SKY. One light with a flat ambient floor leaves every
   ;; face turned away from it at exactly the same brightness, which
   ;; flattens the very geometry the cubes are here to show — two thirds of
   ;; a cube's visible faces read as one dark colour.
   ;;
   ;; A dimmer second light from behind and to the left recovers the
   ;; silhouette edges, and a hemisphere term (bright above, dim below)
   ;; means a face nothing shines on is still ORIENTED rather than merely
   ;; unlit. Cheap: three dot products in a vertex shader that already has
   ;; the normal in hand.
   "  let key  = max(dot(n, normalize(vec3<f32>( 0.45, 0.80,  0.40))), 0.0);\n"
   "  let fill = max(dot(n, normalize(vec3<f32>(-0.55, 0.15, -0.60))), 0.0);\n"
   "  let sky  = 0.26 + 0.20 * (0.5 + 0.5 * n.y);\n"
   "  let shade = sky + 0.56 * key + 0.20 * fill;\n"
   "\n"
   "  // A REAL w, unlike the points shader which divides by hand and emits\n"
   "  // w = 1. Two things need it. Depth: z must survive to the depth buffer,\n"
   "  // and it only does so as a ratio the hardware divides. And\n"
   "  // perspective-correct interpolation, which is derived from w — a\n"
   "  // camera-facing quad has no depth across it to get wrong, but a cube\n"
   "  // does. Homogeneous clipping also handles vertices behind the eye for\n"
   "  // free, so no off-screen sentinel is needed here.\n"
   "  let near = 0.05;\n"
   "  let far = 100.0;\n"
   "  let a = far / (far - near);\n"
   "  let b = -(near * far) / (far - near);\n"
   "  let aspect = u.width / u.height;\n"
   "\n"
   "  var o : VSOut;\n"
   "  o.pos = vec4<f32>(rx * u.fov / aspect, ry * u.fov, a * zc + b, zc);\n"
   "  o.tint = vec3<f32>(pts[base + 4u], pts[base + 5u], pts[base + 6u]) * shade;\n"
   "  return o;\n"
   "}\n"
   "\n"
   "@fragment\n"
   "fn fs(v : VSOut) -> @location(0) vec4<f32> {\n"
   "  return vec4<f32>(v.tint, 1.0);\n"
   "}\n"))
