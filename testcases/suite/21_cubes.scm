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

(test-suite "21_cubes: instanced geometry over the point buffer")

(define (string-contains? haystack needle)
  (let ((hn (string-length haystack)) (nn (string-length needle)))
    (let loop ((i 0))
      (cond ((> (+ i nn) hn) #f)
            ((string=? (substring haystack i (+ i nn)) needle) #t)
            (else (loop (+ i 1)))))))

;;--- the geometry -------------------------------------------------------

;; 6 faces x 2 triangles x 3 vertices. The draw call passes this constant,
;; so a shader array of a different length would read past its end.
(assert-equal "36 vertices per cube" 36 cube-vertex-count)
(assert-true "the shader array is the same length"
             (string-contains? cubes-wgsl
                               (string-append "array<vec3<f32>, "
                                              (number->string cube-vertex-count)
                                              ">")))
(assert-true "six face normals, one per face"
             (string-contains? cubes-wgsl "array<vec3<f32>, 6>"))
(assert-true "the normal is chosen by face, not by vertex"
             (string-contains? cubes-wgsl "norms[vi / 6u]"))

;;--- it must not billboard ----------------------------------------------
;; THE difference from lib/points.scm. A sprite adds its corner offset
;; after projection so it always faces the camera; a cube adds it in world
;; space, before, so it turns with the scene. Get this wrong and you get
;; flat hexagons that never rotate — which looks like a lighting bug.

(assert-true "the offset is applied in world space"
             (string-contains? cubes-wgsl "let world = centre + verts[vi] * half;"))
(assert-true "and the camera rotation is applied AFTER it"
             (string-contains? cubes-wgsl "let rx = world.x * cy + world.z * sy;"))

;;--- a real w -----------------------------------------------------------
;; The points shader divides by hand and emits w = 1, which is fine for
;; something flat and camera-facing. A cube has extent along the view
;; direction, so depth and perspective-correct interpolation both matter,
;; and both come from w.

(assert-true "w is the view depth, not 1"
             (string-contains? cubes-wgsl "vec4<f32>(rx * u.fov / aspect, ry * u.fov, a * zc + b, zc);"))
(assert-false "it does not emit the sprite's w = 1"
              (string-contains? cubes-wgsl "0.0, 1.0);"))
;; Depth must land in [0,1] after the divide, which is WebGPU's convention.
;; OpenGL uses [-1,1], and a projection copied from a GL tutorial is wrong
;; in exactly this one place — it looks fine near the camera and collapses
;; far away.
(assert-true "the depth mapping is the WebGPU one"
             (and (string-contains? cubes-wgsl "let a = far / (far - near);")
                  (string-contains? cubes-wgsl "let b = -(near * far) / (far - near);")))

;;--- shading ------------------------------------------------------------
;; Without it a cube is a flat silhouette and reads as a hexagon. Computed
;; per vertex because all three vertices of a face share a normal, so the
;; interpolated result would be constant anyway.

(assert-true "there is a diffuse term" (string-contains? cubes-wgsl "lambert"))
(assert-true "shading never reaches zero, so unlit faces stay visible"
             (string-contains? cubes-wgsl "0.28 + 0.72 * lambert"))

;;--- the coupling with the point buffer ---------------------------------
;; Same seven floats as lib/points.scm. If the stride drifts, cubes read
;; their neighbours' attributes and nothing errors.

(assert-equal "cubes and sprites share a stride" 7 points-stride)
(assert-true "the shader strides by points-stride"
             (string-contains? cubes-wgsl
                               (string-append "let base = ii * "
                                              (number->string points-stride)
                                              "u;")))
(assert-true "size is read from offset 3"
             (string-contains? cubes-wgsl "let half = pts[base + 3u];"))
(assert-true "colour is read from offsets 4..6"
             (string-contains? cubes-wgsl
                               "vec3<f32>(pts[base + 4u], pts[base + 5u], pts[base + 6u])"))

;; A released pool slot has size 0, which collapses all 36 vertices onto
;; the centre — every triangle degenerate, no fragments. That is what lets
;; the draw always submit the full capacity.
(assert-true "size scales the offset, so size 0 is invisible"
             (string-contains? cubes-wgsl "verts[vi] * half"))

(assert-true "the cube shader indexes by the shared stride"
             (string-contains? cubes-wgsl
                               (string-append "let base = ii * " points-stride-wgsl ";")))

(suite-summary)
