;;----------------------------------------------------------------------
;; Layer 20: the heat ramp, and the fact that there are two of it
;;
;; lib/colour.wgsl runs on the GPU for the wrangle; lib/colour.scm runs on
;; the CPU because actors choose their own colour and actors are Scheme.
;; The same five control points therefore exist in two languages, and
;; duplication drifts.
;;
;; So the load-bearing test here is not that the ramp interpolates — it is
;; that the WGSL file still contains the numbers this file uses. Change one
;; and not the other and a test fails, rather than two demos looking
;; subtly different and each being individually plausible.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")
(load "lib/actors.scm")

(test-suite "20_colour: the heat ramp in two languages")


;;--- the ramp itself ----------------------------------------------------

(assert-equal "five stops of three channels" 15 (vector-length heat-stops))

(assert-equal "t=0 is the cold end"   [0.08 0.04 0.22] (heat-colour 0.0))
(assert-equal "t=0.25 is magenta"     [0.48 0.06 0.32] (heat-colour 0.25))
(assert-equal "t=0.5 is red-orange"   [0.87 0.24 0.1]  (heat-colour 0.5))
(assert-equal "t=0.75 is amber"       [0.99 0.68 0.15] (heat-colour 0.75))
;; The min-against-3 guard. Without it the last segment collapses to its
;; own lower endpoint and the hottest value comes out amber, which looks
;; like a slightly dull core rather than a bug.
(assert-equal "t=1 is white, not amber" [1.0 0.98 0.88] (heat-colour 1.0))

(assert-equal "it interpolates between stops"
              (+ 0.08 (* 0.5 (- 0.48 0.08))) (heat-ref 0.125 0))

;; Out of range is clamped rather than extrapolated: an actor's energy can
;; go slightly negative on its last frame, and a ramp that ran off the end
;; would give it a colour brighter than white.
(assert-equal "below zero clamps" [0.08 0.04 0.22] (heat-colour -5.0))
(assert-equal "above one clamps"  [1.0 0.98 0.88]  (heat-colour 17.0))

;; Cold is dim but never black. Under additive blending a black point does
;; not fade, it vanishes — and takes the silhouette with it.
(assert-true "the cold end is visible" (> (heat-ref 0.0 0) 0.0))

;;--- THE drift guard ----------------------------------------------------
;; Every stop must appear verbatim in the WGSL twin.

(define wgsl-ramp (embedded-source "colour.wgsl"))
(assert-true "colour.wgsl is reachable" (string? wgsl-ramp))

(assert-true "cold violet matches"  (string-contains? wgsl-ramp "vec3<f32>(0.08, 0.04, 0.22)"))
(assert-true "magenta matches"      (string-contains? wgsl-ramp "vec3<f32>(0.48, 0.06, 0.32)"))
(assert-true "red-orange matches"   (string-contains? wgsl-ramp "vec3<f32>(0.87, 0.24, 0.10)"))
(assert-true "amber matches"        (string-contains? wgsl-ramp "vec3<f32>(0.99, 0.68, 0.15)"))
(assert-true "white-hot matches"    (string-contains? wgsl-ramp "vec3<f32>(1.00, 0.98, 0.88)"))
;; And the same guard against the degenerate last segment.
(assert-true "the WGSL twin has the same min-against-3 guard"
             (string-contains? wgsl-ramp "let i = min(floor(s), 3.0);"))

;;--- writing a heat-coloured point --------------------------------------
;; pool-write-heat! exists because computing the segment once instead of
;; three times was worth 1ms a frame across five hundred actors. It must
;; still agree with the ramp it is a fast path for.

;; Values read back out of the buffer have been through f32, so they are
;; NOT the doubles that went in — see layer 17. Comparing them exactly is
;; a mistake this file made on its first run.
(define (close? a b) (< (abs (- a b)) 0.0000001))

(define p (make-point-pool 2))
(define slot (pool-claim! p))
(pool-write-heat! p slot 0.5 0.25 -0.125 0.02 0.75)
(define expect (heat-colour 0.75))
(define v (pool-view p))
(assert-equal "position is written" 0.5 (point-x v slot))
(assert-equal "and depth" -0.125 (point-z v slot))   ; exact: powers of two
(assert-true "red agrees with the ramp"
             (close? (vector-ref expect 0) (view-ref v (+ 4 (* slot points-stride)))))
(assert-true "green agrees"
             (close? (vector-ref expect 1) (view-ref v (+ 5 (* slot points-stride)))))
(assert-true "blue agrees"
             (close? (vector-ref expect 2) (view-ref v (+ 6 (* slot points-stride)))))

;; A dying actor's energy can dip below zero before it notices.
(pool-write-heat! p slot 0.0 0.0 0.0 0.01 -0.2)
(assert-true "negative energy still gives the cold end"
             (close? (heat-ref 0.0 0) (view-ref v (+ 4 (* slot points-stride)))))

(suite-summary)
