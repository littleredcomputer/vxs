;;----------------------------------------------------------------------
;; Layer 17: the instanced-points harness (lib/points.scm)
;;
;; The second harness over the same GPU. lib/shadertoy.scm draws one
;; full-screen quad and computes a colour per pixel; this draws one small
;; quad per POINT and reads its attributes from a storage buffer. Who fills
;; that buffer is deliberately not the harness's business — Scheme does
;; today, fibers will next, a compute wrangle after that.
;;
;; The thing worth testing here is a COUPLING, not an algorithm. The stride
;; of six floats per point exists in two places that cannot see each other:
;; points-stride in Scheme, and the `ii * 6u` indexing inside the WGSL. If
;; they ever disagree, nothing errors — the shader reads whatever floats
;; happen to sit at the wrong offsets, and the picture is simply wrong.
;; That is the same failure mode as struct alignment, which is why the
;; layout is a flat array<f32> rather than an array of structs: WGSL gives
;; vec3<f32> a 16-byte alignment inside a storage array, so a tidy struct
;; would not pack the way the identical fields pack on the host.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")
(load "lib/points.scm")

(test-suite "17_points: instanced point buffers")

(define (string-contains? haystack needle)
  (let ((hn (string-length haystack)) (nn (string-length needle)))
    (let loop ((i 0))
      (cond ((> (+ i nn) hn) #f)
            ((string=? (substring haystack i (+ i nn)) needle) #t)
            (else (loop (+ i 1)))))))

;;--- the buffer ---------------------------------------------------------

(assert-equal "six floats per point" 6 points-stride)

(define b (make-points 4))
(define v (points-view b))

(assert-equal "buffer is stride * 4 bytes per point" 96 (bytes-length b))
(assert-equal "the view sees every float" 24 (view-length v))
(assert-equal "a bound buffer is sealed, not growable"
              :sealed (bytes-residency b))
(assert-equal "the view reads f32" :f32 (view-type v))
(assert-equal "a fresh buffer is zeroed" 0.0 (view-ref v 0))

;;--- attributes land at the right offsets --------------------------------
;; Writing one point must not disturb its neighbours: an off-by-one in the
;; stride shows up here and essentially nowhere else until it reaches a
;; screen.

;; Exactly-representable f32 values, so these assertions test the LAYOUT
;; and not the float format. See the precision test below for the other
;; half of that story.
(point-set! v 0 -0.5 0.25 0.03125 1.0 0.5 0.375)
(point-set! v 3 0.75 -0.5 0.0625 0.125 0.875 0.25)

(assert-equal "x of point 0" -0.5 (point-x v 0))
(assert-equal "y of point 0" 0.25 (point-y v 0))
(assert-equal "x of point 3" 0.75 (point-x v 3))
(assert-equal "y of point 3" -0.5 (point-y v 3))
(assert-equal "point 1 was not touched" 0.0 (point-x v 1))
(assert-equal "point 2 was not touched" 0.0 (point-y v 2))

;; Every field of a point, by raw float index — this is the layout the
;; shader assumes, spelled out.
(assert-equal "size sits at offset 2" 0.03125 (view-ref v 2))
(assert-equal "red sits at offset 3"  1.0     (view-ref v 3))
(assert-equal "green sits at offset 4" 0.5    (view-ref v 4))
(assert-equal "blue sits at offset 5" 0.375   (view-ref v 5))
(assert-equal "point 3 starts at float 18" 0.75 (view-ref v 18))

;;--- the shader agrees with the layout -----------------------------------

(assert-true "declares the storage buffer as a flat float array"
             (string-contains? points-wgsl
                               "@group(0) @binding(1) var<storage, read> pts : array<f32>;"))
(assert-true "draws one instance per point"
             (string-contains? points-wgsl "@builtin(instance_index)"))
(assert-true "builds the quad from vertex_index"
             (string-contains? points-wgsl "@builtin(vertex_index)"))
(assert-true "six quad corners, two triangles"
             (string-contains? points-wgsl "array<vec2<f32>, 6>"))

;; THE coupling: the shader's per-instance stride must equal points-stride.
;; Written this way so that changing points-stride without changing the
;; shader fails here rather than on screen.
(assert-true "the shader strides by points-stride"
             (string-contains? points-wgsl
                               (string-append "let base = ii * "
                                              (number->string points-stride)
                                              "u;")))

;; And the field offsets the shader reads, same argument.
(assert-true "shader reads x at +0" (string-contains? points-wgsl "pts[base + 0u]"))
(assert-true "shader reads size at +2" (string-contains? points-wgsl "pts[base + 2u]"))
(assert-true "shader reads blue at +5" (string-contains? points-wgsl "pts[base + 5u]"))

;; A struct would have been tidier and wrong; make sure nobody quietly
;; reintroduces one.
(assert-false "no vec3 inside the storage array"
              (string-contains? points-wgsl "array<vec3<f32>>"))

;;--- f32 is not a double --------------------------------------------------
;; Scheme numbers here are doubles; the buffer stores f32. A value that
;; is not exactly representable in 24 bits of mantissa comes back CHANGED,
;; and comparing it for equality against what you wrote will fail. Worth
;; stating: it is a real trap when a test — or a simulation — reads back
;; what it just stored and expects it unaltered.
(define pv (points-view (make-points 1)))
(view-set! pv 0 0.1)
(assert-false "0.1 does not survive a round trip through f32"
              (= 0.1 (view-ref pv 0)))
(assert-true "but it is close"
             (< (abs (- 0.1 (view-ref pv 0))) 0.0000001))
(view-set! pv 1 0.25)
(assert-equal "a power-of-two fraction survives exactly" 0.25 (view-ref pv 1))

;;--- sizing -------------------------------------------------------------

(assert-equal "an empty buffer is legal" 0 (bytes-length (make-points 0)))
(assert-equal "1000 points" 24000 (bytes-length (make-points 1000)))

(suite-summary)
