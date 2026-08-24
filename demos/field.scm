;;; ==========================================================
;;; Cube grid over a sliding noise field
;;;
;;; The grid never moves. The FIELD slides under it, and each
;;; cube reads whatever is passing beneath at that moment —
;;; so the motion you see is entirely in colour and size,
;;; with every cube fixed in space.
;;;
;;; All of it runs on the GPU. The grid is uploaded once; a
;;; compute wrangle rewrites colour and size each frame from
;;; gradient noise sampled at the cube's own position, and the
;;; same buffer is then drawn as solid geometry.
;;; ==========================================================

;;; Loaded explicitly, so this demo is self-contained and so its
;;; declarations start from a known state: lib/wrangle.scm re-runs its
;;; (define scratch-attrs '()) on load, and a demo that inherited an
;;; earlier one's attributes would emit a scratch binding nothing binds.
;;; Seven of the ten demos already did this; these two relied on declaring
;;; everything themselves, which works until one of them does not.
(load "lib/gpu.scm")

(define SIDE 24)                       ; 24^3 = 13,824 cubes
(define N (* SIDE SIDE SIDE))
(define SPACING (/ 2.6 SIDE))

;;; Seed the grid once. Positions are written here and never
;;; touched again — the wrangle rewrites size and colour and
;;; hands the position straight back.
(define grid (make-points N))
(define gv (points-view grid))

(let loop ((i 0))
  (if (< i N)
      (let* ((x (modulo i SIDE))
             (y (modulo (quotient i SIDE) SIDE))
             (z (quotient i (* SIDE SIDE)))
             (c (/ (- SIDE 1) 2.0)))
        (point-set! gv i
                    (* SPACING (- x c))
                    (* SPACING (- y c))
                    (* SPACING (- z c))
                    0.0 0.0 0.0 0.0)
        (loop (+ i 1)))))

;;; Live knobs. Every one of these is a uniform slot, so
;;; turning any of them costs nothing — no recompile, no
;;; second kernel, no 'if (mode > 0.5)'.
;;; Orientation is a STOCK attribute: an attribute named 'pose of type
;;; :quat is the convention the cube renderer looks for, so declaring it
;;; is the whole of turning the cubes on.
;;; `pose` turns each body; `shape` picks which solid it is. Both are stock
;;; attributes the cube renderer looks for by name — declaring `shape` is
;;; the whole of enabling the other two solids.
(scratch-attributes! '((pose :quat) (shape :u32)))
(define SCRATCH (make-scratch N))

(wrangle-params! '(scale drift gain floor twist
                   (field-seed :u32) (solid :u32) (warm :flag)))

(define P (make-wrangle-params))
(define PV (wrangle-params-view P))
(param-set! PV 'scale 2.4)             ; noise cells per unit
(param-set! PV 'drift 0.19)            ; how fast the field slides
(param-set! PV 'gain  0.55)            ; field magnitude -> cube size
(param-set! PV 'floor 0.12)            ; smallest cube, as a fraction
(param-set! PV 'field-seed 20260822)
;;; 0 tetrahedron, 1 octahedron, 2 cube. Live from the REPL:
;;;   (param-set! PV 'solid 1)
(param-set! PV 'solid 2)
(param-set! PV 'twist 1.7)             ; field magnitude -> radians
(param-set! PV 'warm  #f)

;;; --- the kernel, in Scheme -------------------------------------------
;;; lib/wgsl.scm compiles this and type-checks it first, so a vec2 handed
;;; to something expecting a vec3 — or an attribute given the wrong type —
;;; is an error here with the form in hand, rather than a shader
;;; compilation log naming a line of generated text.
;;;
;;; wrangle-wgsl still exists and is still the escape hatch. This is the
;;; same kernel it used to hold.
(define kernel
  (wrangle-scheme
   '(let* (;; The field slides; the grid does not. Sampling at the cube's
           ;; own position plus a time offset is what makes the structure
           ;; appear to move THROUGH the lattice rather than with it.
           (q    (+ (* position scale)
                    (vec3 (* time drift) (* time drift 0.37) 0.0)))
           (f    (perlin3v q field-seed))
           (mag  (length f))
           (dir  (/ f (max mag 0.000001)))
           (t    (clamp mag 0.0 1.0))

           ;; Size from magnitude, with a floor so nothing vanishes: an
           ;; empty cell reads as a lull rather than a hole.
           ;;
           ;; THIS COEFFICIENT HAS TWO REGIMES, and the crossover is the
           ;; grid spacing. Well under it (0.1, here) every cube stands
           ;; alone and the field reads as texture — each cube is its own
           ;; sample of it.
           ;;
           ;; Well over it (try 0.5) each cube reaches several spacings and
           ;; swallows its neighbours, so all that survives to be seen is
           ;; the local MAXIMA of the magnitude field. That is a
           ;; morphological dilation, and it looks like architecture: flat
           ;; slabs, hard occlusion edges, structure at a far coarser scale
           ;; than the lattice. Same field, same seed.
           (half (* (+ floor (* gain mag)) 0.1))

           ;; Colour from DIRECTION, not magnitude. The two carry different
           ;; information and mapping both to one channel throws half of it
           ;; away — direction gives the field its grain, magnitude its
           ;; weather.
           ;;
           ;; `if` is a SELECTION here, not a branch: both arms evaluate
           ;; and the condition picks. Harmless for two pure colour ramps,
           ;; and worth knowing before putting a random draw in one.
           (col  (if warm (heat-colour t) (+ 0.5 (* 0.5 dir)))))

      ;; POSE. The field vector IS a rotation vector — axis f/|f|, angle
      ;; |f| * twist — continuous everywhere including f = 0, where the
      ;; axis stops meaning anything exactly as the angle reaches zero.
      ;; Aiming an axis at the field instead would have to choose a roll,
      ;; and no continuous choice exists on a sphere, so it would snap.
      ;;
      ;; Converted to a quaternion HERE, once per cube. The renderer
      ;; applies it 36 times, once per vertex, and q_rot is two cross
      ;; products with no trigonometry — so the sin and cos happen once
      ;; rather than thirty-six times.
      (point position half (* col (+ 0.35 (* 0.65 t)))
             (pose (q-from-rotvec (* f twist)))
             ;; 0 tetrahedron, 1 octahedron, 2 cube. A :u32 knob rather
             ;; than a constant, so it rides in the uniform and switching
             ;; costs nothing — no recompile, no second kernel.
             (shape solid)))))

(define cam (make-camera))
(camera-distance-set! cam 3.4)
(define (frame! t) (orbit-camera! cam))

(run-wrangle-loop grid N kernel frame! cam
                  :canvas "vxs-gpu-canvas"
                  :params P
                  :scratch SCRATCH
                  :draw :cubes)
