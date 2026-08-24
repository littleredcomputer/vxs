;;----------------------------------------------------------------------
;; Layer 21: map cube (lib/cubes.scm)
;;
;; The same point buffer and the same seven floats, drawn as solid
;; geometry instead of sprites. Nothing here runs WGSL, so these are
;; structural assertions — they catch a vertex count that stopped matching
;; its draw call, a stride that drifted from the renderer, or a shader that
;; quietly went back to billboarding. They cannot catch a cube that
;; compiles and looks wrong; that still needs eyes.
;;
;; What the tests are really guarding is the set of differences between a
;; sprite and a cube, because each one is easy to half-apply:
;;
;;   world-space offset, not clip-space  — else it billboards and the
;;                                         "cubes" are flat hexagons
;;   a real w, not 1                     — else there is no depth to test
;;                                         and no perspective correction
;;   depth testing, not additive blend   — else the far cubes draw over
;;                                         the near ones
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")
(load "lib/cubes.scm")

;; cubes-wgsl is a procedure now: the shader depends on whether a stock
;; `pose` attribute was declared.
(define csrc (cubes-wgsl))

(test-suite "21_cubes: instanced geometry over the point buffer")


;;--- the geometry -------------------------------------------------------

;; 6 faces x 2 triangles x 3 vertices. The draw call passes this constant,
;; so a shader array of a different length would read past its end.
(assert-equal "36 vertices per cube" 36 cube-vertex-count)
;; THREE SOLIDS IN ONE TABLE, and the cube is the largest, so it still
;; sets the draw's vertex count. A tetrahedron uses the first 12 entries
;; and an octahedron the next 24; the rest collapse onto the centre, which
;; is the same degenerate-triangle trick a released slot already relied on.
(assert-equal "the shapes total 72 vertices" 72 shape-total)
(assert-equal "and the cube is the largest, so it sets the draw"
              cube-vertex-count (caddr shape-counts))
(assert-equal "offsets follow the counts" '(0 12 36) shape-offsets)
(assert-true "the shader array holds all of them"
             (string-contains? csrc
                               (string-append "array<vec3<f32>, "
                                              (number->string shape-total) ">")))
;; ONE NORMAL PER VERTEX, not per face. The three solids have 3, 3 and 6
;; vertices per face, so no single divisor can serve them all.
(assert-true "there is a normal for every vertex"
             (string-contains? csrc
                               (string-append "var shape_nrm = array<vec3<f32>, "
                                              (number->string shape-total) ">")))
(assert-true "and it is indexed the same way the position is"
             (string-contains? csrc "shape_nrm[idx], live)"))

;; Normals are centroid directions, so every one points AWAY from the
;; origin by construction and winding never has to be got right. A face
;; wound backwards would light backwards rather than fail, which is the
;; kind of wrong that survives review.
(assert-true "every normal is unit length"
             (let loop ((ns shape-nrm))
               (cond ((null? ns) #t)
                     ((> (abs (- 1.0 (sqrt (+ (* (caar ns) (caar ns))
                                              (* (cadar ns) (cadar ns))
                                              (* (caddar ns) (caddar ns))))))
                         1e-9) #f)
                     (else (loop (cdr ns))))))
;; POINTING OUTWARD IS NOT ENOUGH, and asking only that let a real bug
;; through. The centroid direction was used first, which is correct for a
;; solid whose faces ARE triangles but wrong for the cube: a square face is
;; split into two, and each half-triangle's centroid points off-axis. The
;; x = -1 face got (-0.905, 0.302, -0.302) instead of (-1, 0, 0), so both
;; halves of every square shaded differently and each face was creased
;; along its diagonal — cubes that looked faceted. Every one of those
;; normals still pointed outward.
;;
;; A cube's faces are axis-aligned, so its normals must be too. That is the
;; assertion the earlier one should have been.
(assert-true "every cube normal is axis-aligned"
             (let loop ((k (caddr shape-offsets)))  ; the cube's slice, third of three
               (cond ((>= k shape-total) #t)
                     ((< (max (abs (car (list-ref shape-nrm k)))
                              (abs (cadr (list-ref shape-nrm k)))
                              (abs (caddr (list-ref shape-nrm k)))) 0.9999) #f)
                     (else (loop (+ k 1))))))

(assert-true "and points outward from the vertex it belongs to"
             (let loop ((ps shape-pos) (ns shape-nrm))
               (cond ((null? ps) #t)
                     ((<= (+ (* (caar ps) (caar ns))
                             (* (cadar ps) (cadar ns))
                             (* (caddar ps) (caddar ns))) 0.0) #f)
                     (else (loop (cdr ps) (cdr ns))))))

;;--- it must not billboard ----------------------------------------------
;; THE difference from lib/points.scm. A sprite adds its corner offset
;; after projection so it always faces the camera; a cube adds it in world
;; space, before, so it turns with the scene. Get this wrong and you get
;; flat hexagons that never rotate — which looks like a lighting bug.

(assert-true "the offset is applied in world space"
             (string-contains? csrc "let world = centre + lv * half;"))
(assert-true "and the camera rotation is applied AFTER it"
             (string-contains? csrc "let rx = world.x * cy + world.z * sy;"))

;;--- a real w -----------------------------------------------------------
;; The points shader divides by hand and emits w = 1, which is fine for
;; something flat and camera-facing. A cube has extent along the view
;; direction, so depth and perspective-correct interpolation both matter,
;; and both come from w.

(assert-true "w is the view depth, not 1"
             (string-contains? csrc "vec4<f32>(rx * u.fov / aspect, ry * u.fov, a * zc + b, zc);"))
(assert-false "it does not emit the sprite's w = 1"
              (string-contains? csrc "0.0, 1.0);"))
;; Depth must land in [0,1] after the divide, which is WebGPU's convention.
;; OpenGL uses [-1,1], and a projection copied from a GL tutorial is wrong
;; in exactly this one place — it looks fine near the camera and collapses
;; far away.
(assert-true "the depth mapping is the WebGPU one"
             (and (string-contains? csrc "let a = far / (far - near);")
                  (string-contains? csrc "let b = -(near * far) / (far - near);")))

;;--- shading ------------------------------------------------------------
;; Without it a cube is a flat silhouette and reads as a hexagon. Computed
;; per vertex because all three vertices of a face share a normal, so the
;; interpolated result would be constant anyway.

;; KEY, FILL AND SKY. One light against a flat ambient floor leaves every
;; face turned away from it at exactly the same brightness, which flattens
;; the geometry the cubes exist to show — two thirds of a cube's visible
;; faces read as one dark colour. A dimmer second light recovers the
;; silhouette edges, and a hemisphere term means a face nothing shines on
;; is still ORIENTED rather than merely unlit.
(assert-true "there is a key light"
             (string-contains? csrc "let key  = max(dot(n,"))
(assert-true "and a dimmer fill from another direction"
             (string-contains? csrc "let fill = max(dot(n,"))
(assert-true "and a hemisphere term, so an unlit face still has an up and a down"
             (string-contains? csrc "let sky  = 0.26 + 0.20 * (0.5 + 0.5 * n.y);"))
(assert-true "shading never reaches zero, so unlit faces stay visible"
             (string-contains? csrc "let shade = sky + 0.56 * key + 0.20 * fill;"))

;; A RIM is the cheap half of glow: real bloom wants a second pass, a
;; render target and a blur, while this is one dot product against the
;; view direction. It also does something bloom does not — it draws the
;; EDGE of every body, so a crowd of them reads as many objects rather
;; than one mass.
(assert-true "the silhouette lights up"
             (string-contains? csrc
               "let rim = pow(1.0 - abs(dot(normalize(ncam), vdir)), 2.5);"))
;; The normal must reach camera space to be compared with the view
;; direction, by the same yaw-then-pitch the position takes. Comparing a
;; world normal against a camera-space view vector would put the rim in the
;; wrong place and swing it as the camera orbits.
(assert-true "and the normal is brought into camera space to find it"
             (string-contains? csrc "let ncam = vec3<f32>(nx, n.y * cp - nz0 * sp,"))
;; Gamma below one lifts midtones without touching the ends. A linear
;; multiply would clip the highlights first and desaturate them, which
;; makes a scene whiter rather than brighter.
(assert-true "midtones are lifted, not multiplied"
             (string-contains? csrc "vec3<f32>(0.80));"))

;;--- the coupling with the point buffer ---------------------------------
;; Same seven floats as lib/points.scm. If the stride drifts, cubes read
;; their neighbours' attributes and nothing errors.

(assert-equal "cubes and sprites share a stride" 7 points-stride)
(assert-true "the shader strides by points-stride"
             (string-contains? csrc
                               (string-append "let base = ii * "
                                              (number->string points-stride)
                                              "u;")))
(assert-true "size is read from offset 3"
             (string-contains? csrc "let half = pts[base + 3u];"))
(assert-true "colour is read from offsets 4..6"
             (string-contains? csrc
                               "vec3<f32>(pts[base + 4u], pts[base + 5u], pts[base + 6u])"))

;; A released pool slot has size 0, which collapses all 36 vertices onto
;; the centre — every triangle degenerate, no fragments. That is what lets
;; the draw always submit the full capacity.
(assert-true "size scales the offset, so size 0 is invisible"
             (string-contains? csrc "lv * half"))

(assert-true "the cube shader indexes by the shared stride"
             (string-contains? csrc
                               (string-append "let base = ii * " points-stride-wgsl ";")))

;;--- pose is a stock attribute ------------------------------------------
;; An attribute named `pose` of type :quat is the convention. Declaring one
;; is the whole of turning the cubes on; declaring nothing must leave the
;; shader byte-for-byte as it was, because an unused storage binding is
;; still a different pipeline layout.

(scratch-attributes! '())
(assert-false "with no pose declared, no third binding appears"
              (string-contains? (cubes-wgsl) "binding(2)"))
(assert-false "and nothing rotates"
              (string-contains? (cubes-wgsl) "q_rot"))
(assert-equal "the shader is exactly what it was" csrc (cubes-wgsl))

(scratch-attributes! '((charge :f32) (pose :quat)))
(define psrc (cubes-wgsl))
(assert-true "declaring a pose binds the scratch buffer, read-only"
             (string-contains? psrc
               "@group(0) @binding(2) var<storage, read> scratch : array<f32>;"))
(assert-true "and brings the quaternion library with it"
             (string-contains? psrc "fn q_rot(q : vec4<f32>, v : vec3<f32>)"))
;; The accessor's stride and offset come from the SAME declaration the
;; kernel compiles against, so the two cannot disagree about where a pose
;; lives — the failure that would otherwise read as cubes turning wrongly.
;; GETTER ONLY. The renderer binds scratch var<storage, read>, so a setter
;; is not merely unused there — it does not compile:
;;   cannot store into a read-only type 'ref<storage, f32, read>'
;; The kernel, which binds the same buffer read_write, gets both.
(assert-true "the renderer can read a pose"
             (string-contains? psrc "fn attr_pose(i : u32) -> vec4<f32>"))
(assert-false "and cannot write one, because its binding is read-only"
              (string-contains? psrc "attr_pose_set"))
(assert-true "while the kernel gets both halves"
             (string-contains? (wrangle-wgsl "") "fn attr_pose_set(i : u32, v : vec4<f32>)"))

(assert-true "the accessor derives its offset from the declaration"
             (string-contains? psrc "let b = i * 5u + 1u;"))
;; Rotate the corner about the cube's OWN centre, then place it. The other
;; order swings the cube around the origin.
(assert-true "corners rotate about the centre, then translate"
             (string-contains? psrc
               "let world = centre + q_rot(attr_pose(ii), lv * half);"))
;; Rotating geometry without the normal leaves shading fixed to the world
;; while the faces move under it — which reads as a lighting bug.
(assert-true "and the normal turns with them"
             (string-contains? psrc "let n = q_rot(attr_pose(ii), bn);"))

;;--- shapes are opt-in too ----------------------------------------------
(scratch-attributes! '((shape :u32)))
(define ssrc (cubes-wgsl))
(assert-true "a declared shape attribute selects per instance"
             (string-contains? ssrc "let sh = min(attr_shape(ii), 2u);"))
(assert-true "and vertices past that shape's count collapse to the centre"
             (string-contains? ssrc "let live = vi < shape_cnt[sh];"))
(scratch-attributes! '())
(assert-true "declaring none pins every instance to the cube"
             (string-contains? (cubes-wgsl) "let sh = 2u;"))

;;--- the quaternion convention ------------------------------------------
;; (x y z w), scalar LAST, which is what makes q.xyz and q.w read cleanly
;; and matches the posquat order px py pz qx qy qz qw.
(assert-true "the identity is scalar-last"
             (string-contains? (embedded-source "quat.wgsl")
                               "return vec4<f32>(0.0, 0.0, 0.0, 1.0);"))
;; q_rot must stay free of transcendentals: it is the one that runs once
;; per vertex, 36 times per cube, and nothing amortises across them.
(define qsrc (embedded-source "quat.wgsl"))
(assert-true "q_rot is two crosses, and no trigonometry at all"
             (string-contains? qsrc
               "  let t = 2.0 * cross(q.xyz, v);\n  return v + q.w * t + cross(q.xyz, t);\n"))
;; And q_from_rotvec must handle zero, where the axis stops being defined
;; at exactly the moment the angle vanishes.
(assert-true "q_from_rotvec returns the identity at zero"
             (string-contains? qsrc "if (t < 1e-8) { return q_identity(); }"))

(scratch-attributes! '())
(suite-summary)
