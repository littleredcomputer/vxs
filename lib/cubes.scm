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


;;--- the shape table ----------------------------------------------------
;; Three solids in one draw call. An instance names a shape and the vertex
;; shader reads its slice of a combined table; vertices past that shape's
;; count collapse onto the centre, which is the same degenerate-triangle
;; trick the cube already used for its own tail.
;;
;; So a tetrahedron costs 24 wasted vertices out of 36 and no extra draw,
;; no second pipeline, and no per-shape instance sorting. The largest shape
;; sets the count and the smaller ones ride along.
;;
;; GENERATED, not written out. A hundred and forty-four vectors typed by
;; hand is a hundred and forty-four chances to transpose a sign, and the
;; failure would be one dark facet nobody notices.
;;
;; Normals are the TRIANGLE's own normal, oriented outward.
;;
;; The centroid direction was tried first and is wrong for the cube. It is
;; correct for a solid whose faces ARE triangles, but a square face is
;; split into two, and each half-triangle's centroid points off-axis: the
;; x = -1 face gave (-0.905, 0.302, -0.302) instead of (-1, 0, 0). Both
;; halves of every square then shade differently and each face is creased
;; along its diagonal, which makes a cube look faceted — visible
;; immediately, and passing a test that only asked whether the normal
;; pointed outward.
;;
;; The cross product gives the true face normal but depends on winding.
;; Rather than get winding right in three vertex lists, the centroid is
;; kept as the ORIENTATION reference: flip the cross product if it
;; disagrees. Correct for any triangle of a solid centred on the origin,
;; whatever order its corners are in.

(define (v3n x y z)
  (let ((m (sqrt (+ (* x x) (* y y) (* z z)))))
    (list (/ x m) (/ y m) (/ z m))))

;; Circumradius sqrt(3) for all three, matching the cube's corners, so a
;; shape swap changes the silhouette without changing the scale.
(define shape-R 1.7320508075688772)

(define tetra-verts
  (list (list 1.0 1.0 1.0) (list 1.0 -1.0 -1.0)
        (list -1.0 1.0 -1.0) (list -1.0 -1.0 1.0)))
(define tetra-faces '((0 1 2) (0 2 3) (0 3 1) (1 3 2)))

(define octa-verts
  (let ((a shape-R))
    (list (list a 0.0 0.0) (list (- a) 0.0 0.0)
          (list 0.0 a 0.0) (list 0.0 (- a) 0.0)
          (list 0.0 0.0 a) (list 0.0 0.0 (- a)))))
(define octa-faces '((0 2 4) (0 4 3) (0 3 5) (0 5 2)
                     (1 4 2) (1 3 4) (1 5 3) (1 2 5)))

(define cube-corners
  (let loop ((i 0) (acc '()))
    (if (= i 8)
        (reverse acc)
        (loop (+ i 1)
              (cons (list (if (= 0 (modulo i 2)) -1.0 1.0)
                          (if (= 0 (modulo (quotient i 2) 2)) -1.0 1.0)
                          (if (= 0 (quotient i 4)) -1.0 1.0))
                    acc)))))
;; Six quads, each as two triangles, wound consistently outward.
(define cube-faces
  '((0 2 6) (0 6 4) (1 5 7) (1 7 3)
    (0 4 5) (0 5 1) (2 3 7) (2 7 6)
    (0 1 3) (0 3 2) (4 6 7) (4 7 5)))

;; -> (positions . normals), both flat lists of 3-element lists.
(define (v3sub a b) (list (- (car a) (car b)) (- (cadr a) (cadr b)) (- (caddr a) (caddr b))))
(define (v3cross a b)
  (list (- (* (cadr a) (caddr b)) (* (caddr a) (cadr b)))
        (- (* (caddr a) (car b)) (* (car a) (caddr b)))
        (- (* (car a) (cadr b)) (* (cadr a) (car b)))))
(define (v3dot a b) (+ (* (car a) (car b)) (* (cadr a) (cadr b)) (* (caddr a) (caddr b))))

(define (shape-triangles verts faces)
  (let loop ((fs faces) (ps '()) (ns '()))
    (if (null? fs)
        (cons (reverse ps) (reverse ns))
        (let* ((f (car fs))
               (a (list-ref verts (car f)))
               (b (list-ref verts (cadr f)))
               (c (list-ref verts (caddr f)))
               (x (v3cross (v3sub b a) (v3sub c a)))
               ;; The centroid points out of the solid, so it settles which
               ;; way the cross product should face without anyone having to
               ;; wind the vertex lists consistently.
               (ctr (list (+ (car a) (car b) (car c))
                          (+ (cadr a) (cadr b) (cadr c))
                          (+ (caddr a) (caddr b) (caddr c))))
               (o (if (< (v3dot x ctr) 0.0) (list (- (car x)) (- (cadr x)) (- (caddr x))) x))
               (n (v3n (car o) (cadr o) (caddr o))))
          (loop (cdr fs) (cons c (cons b (cons a ps)))
                (cons n (cons n (cons n ns))))))))

(define tetra-tris (shape-triangles tetra-verts tetra-faces))
(define octa-tris  (shape-triangles octa-verts octa-faces))
(define cube-tris  (shape-triangles cube-corners cube-faces))

(define shape-pos (append (car tetra-tris) (car octa-tris) (car cube-tris)))
(define shape-nrm (append (cdr tetra-tris) (cdr octa-tris) (cdr cube-tris)))
(define shape-counts (list (length (car tetra-tris))
                           (length (car octa-tris))
                           (length (car cube-tris))))
(define shape-offsets
  (list 0 (car shape-counts) (+ (car shape-counts) (cadr shape-counts))))
(define shape-total (length shape-pos))

;;--- the table as a BUFFER -----------------------------------------------
;; Six floats per vertex — position then normal — in one flat array, the
;; same shape the point buffer uses and for the same reason: a vec3 in a
;; storage array carries 16-byte alignment, so a packed layout has to be
;; indexed by hand.
;;
;; It lived as a function-local `var` array, which in WGSL is PER
;; INVOCATION. At three solids that is about 1.7KB of private storage per
;; vertex; a dodecahedron alone is 108 vertices, and five solids would be
;; ~5.7KB per invocation, which spills and takes occupancy with it — a
;; frame-rate cliff rather than an error. In a buffer it is read once,
;; shared across invocations and cached, and the shader stops carrying it.
;;
;; Which is also what makes cylinders, cones and arrows a matter of
;; appending rows rather than a decision.
(define shape-vertex-stride 6)

(define (shape-table-bytes)
  (let* ((n (length shape-pos))
         (b (make-bytes (* n shape-vertex-stride 4)))
         (v (bytes-view b :f32)))
    (let loop ((ps shape-pos) (ns shape-nrm) (k 0))
      (if (null? ps)
          (begin (bytes-seal! b) b)
          (let ((o (* k shape-vertex-stride)))
            (view-set! v (+ o 0) (car (car ps)))
            (view-set! v (+ o 1) (cadr (car ps)))
            (view-set! v (+ o 2) (caddr (car ps)))
            (view-set! v (+ o 3) (car (car ns)))
            (view-set! v (+ o 4) (cadr (car ns)))
            (view-set! v (+ o 5) (caddr (car ns)))
            (loop (cdr ps) (cdr ns) (+ k 1)))))))

(define (shape-stride-wgsl)
  (string-append (number->string shape-vertex-stride) "u"))

(define (wgsl-vec3-list vs)
  (wgsl-join
   (map (lambda (v)
          (string-append "    vec3<f32>(" (wgsl-number (car v)) ", "
                         (wgsl-number (cadr v)) ", "
                         (wgsl-number (caddr v)) ")"))
        vs)
   ",\n"))

(define (wgsl-u32-list ns)
  (wgsl-join (map (lambda (n) (string-append (number->string n) "u")) ns) ", "))

;; Only the offsets and counts stay in the shader — two small integers per
;; solid, which stays trivial however many solids there are.
(define (shape-table-wgsl)
  (string-append
   "  var shape_off = array<u32, " (number->string (length shape-offsets)) ">("
   (wgsl-u32-list shape-offsets) ");\n"
   "  var shape_cnt = array<u32, " (number->string (length shape-counts)) ">("
   (wgsl-u32-list shape-counts) ");\n"))

;; The binding and its two accessors, emitted once at file scope.
(define (shape-buffer-wgsl)
  (string-append
   "@group(0) @binding(3) var<storage, read> shapes : array<f32>;\n"
   "fn shape_pos(k : u32) -> vec3<f32> {\n"
   "  let b = k * " (shape-stride-wgsl) ";\n"
   "  return vec3<f32>(shapes[b], shapes[b + 1u], shapes[b + 2u]);\n"
   "}\n"
   "fn shape_nrm(k : u32) -> vec3<f32> {\n"
   "  let b = k * " (shape-stride-wgsl) ";\n"
   "  return vec3<f32>(shapes[b + 3u], shapes[b + 4u], shapes[b + 5u]);\n"
   "}\n"))

;; An instance names its shape through a declared :u32 attribute called
;; `shape`. Declaring none means every instance is a cube, which is what
;; every program written before this expected.
(define (cubes-shaped?)
  (let ((a (assq 'shape scratch-attrs)))
    (and a (eq? (cadr a) :u32))))

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
   (shape-buffer-wgsl)
   ;; The SAME buffer the compute pass writes, bound here read-only. No
   ;; second copy, no upload, no synchronisation to get wrong — the kernel
   ;; sets a pose and the renderer reads it out of the slot it was put in.
   ;; Bound when EITHER attribute is declared, and each accessor emitted
   ;; only for the one that was — a getter for an attribute nobody declared
   ;; would reference an offset that does not exist.
   (if (or (cubes-posed?) (cubes-shaped?))
       (string-append
        "@group(0) @binding(2) var<storage, read> scratch : array<f32>;\n"
        (if (cubes-posed?)
            (string-append
             (scratch-accessors 'pose :quat (scratch-offset 'pose) #t)
             (embedded-source "quat.wgsl") "\n")
            "")
        (if (cubes-shaped?)
            (scratch-accessors 'shape :u32 (scratch-offset 'shape) #t)
            ""))
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
   (shape-table-wgsl)
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
   ;; Pick this instance's shape, and collapse the vertices it does not
   ;; use onto the centre — a zero-area triangle the rasteriser discards.
   ;; That is how three solids share one draw call and one vertex count.
   (if (cubes-shaped?)
       "  let sh = min(attr_shape(ii), 2u);\n"
       "  let sh = 2u;\n")
   "  let live = vi < shape_cnt[sh];\n"
   "  let idx = shape_off[sh] + vi;\n"
   "  let lv = select(vec3<f32>(0.0, 0.0, 0.0), shape_pos(idx), live);\n"
   ;; Rotate the corner about the shape's own centre, then place it. The
   ;; other order would swing it around the origin instead.
   (if (cubes-posed?)
       "  let world = centre + q_rot(attr_pose(ii), lv * half);\n"
       "  let world = centre + lv * half;\n")
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
   ;; One normal PER VERTEX rather than per face, because the three solids
   ;; have different vertices-per-face (3, 3 and 6) and a single divisor
   ;; cannot serve them all.
   "  let bn = select(vec3<f32>(0.0, 1.0, 0.0), shape_nrm(idx), live);\n"
   ;; The NORMAL turns too. Rotating the geometry and not the normal
   ;; leaves the shading fixed to the world while the faces move under it,
   ;; which reads as a lighting bug rather than as a pose.
   (if (cubes-posed?)
       "  let n = q_rot(attr_pose(ii), bn);\n"
       "  let n = bn;\n")
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
   ;; A RIM, which is the cheap half of glow. Real bloom needs a second
   ;; pass, a render target and a blur; this needs one dot product. The
   ;; silhouette of a solid — where its surface turns away from the eye —
   ;; lights up, and against a black background that is what glowing looks
   ;; like. It also does something bloom does not: it draws the EDGE of
   ;; every body, so a cloud of them reads as many objects rather than one
   ;; mass.
   ;;
   ;; The normal has to come into camera space to be compared with the view
   ;; direction, by the same yaw-then-pitch the position already took.
   "  let nx = n.x * cy + n.z * sy;\n"
   "  let nz0 = n.z * cy - n.x * sy;\n"
   "  let ncam = vec3<f32>(nx, n.y * cp - nz0 * sp, nz0 * cp + n.y * sp);\n"
   "  let vdir = normalize(vec3<f32>(rx, ry, zc));\n"
   "  let rim = pow(1.0 - abs(dot(normalize(ncam), vdir)), 2.5);\n"
   "\n"
   "  let hue = vec3<f32>(pts[base + 4u], pts[base + 5u], pts[base + 6u]);\n"
   ;; Gamma below one lifts the midtones without touching the ends, which
   ;; is what makes a dim scene brighter rather than merely whiter — a
   ;; linear multiply would clip the highlights first and desaturate them.
   "  o.tint = pow(hue * shade + hue * rim * 0.85, vec3<f32>(0.80));\n"
   "  return o;\n"
   "}\n"
   "\n"
   "@fragment\n"
   "fn fs(v : VSOut) -> @location(0) vec4<f32> {\n"
   "  return vec4<f32>(v.tint, 1.0);\n"
   "}\n"))
