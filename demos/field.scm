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
(scratch-attributes! '((pose :quat)))
(define SCRATCH (make-scratch N))

(wrangle-params! '(scale drift gain floor twist (field-seed :u32) (warm :flag)))

(define P (make-wrangle-params))
(define PV (wrangle-params-view P))
(param-set! PV 'scale 2.4)             ; noise cells per unit
(param-set! PV 'drift 0.19)            ; how fast the field slides
(param-set! PV 'gain  0.55)            ; field magnitude -> cube size
(param-set! PV 'floor 0.12)            ; smallest cube, as a fraction
(param-set! PV 'field-seed 20260822)
(param-set! PV 'twist 1.7)             ; field magnitude -> radians
(param-set! PV 'warm  #f)

(define kernel (wrangle-wgsl "
  let p = pt_pos(i);

  // The field slides; the grid does not. Sampling at the cube's
  // own position plus a time offset is what makes the structure
  // appear to move THROUGH the lattice rather than with it.
  let q = p * w.p0 + vec3<f32>(w.time * w.p1, w.time * w.p1 * 0.37, 0.0);
  let f = perlin3v(q, w.i0);

  let mag = length(f);
  let dir = f / max(mag, 1e-6);

  // Size from magnitude, with a floor so nothing vanishes: an
  // empty cell reads as a lull rather than a hole.
  //
  // THIS COEFFICIENT HAS TWO REGIMES, and the crossover is the grid
  // spacing. Well under it (0.1, here) every cube stands alone and the
  // field reads as texture — each cube is its own sample of it.
  //
  // Well over it (try 0.5) each cube reaches several spacings and swallows
  // its neighbours, so all that survives to be seen is the local MAXIMA of
  // the magnitude field. That is a morphological dilation, and it looks
  // like architecture: flat slabs, hard occlusion edges, and structure at
  // a far coarser scale than the lattice. Same field, same seed.
  let half = (w.p3 + w.p2 * mag) * 0.1;

  // Colour from DIRECTION, not magnitude. The two carry different
  // information and mapping both to one channel throws half of it
  // away — direction gives the field its grain, magnitude its
  // weather.
  var col = 0.5 + 0.5 * dir;
  if (flag_warm()) { col = heat_colour(clamp(mag, 0.0, 1.0)); }

  // POSE. The field vector IS a rotation vector — axis f/|f|, angle
  // |f| * twist — which is continuous everywhere including f = 0, where
  // the axis stops meaning anything exactly as the angle reaches zero.
  // Aiming an axis at the field instead would have to choose a roll, and
  // no continuous choice exists on a sphere, so it would snap somewhere.
  //
  // Converted to a quaternion HERE, once per cube. The renderer applies it
  // 36 times, once per vertex, and q_rot is two cross products with no
  // trigonometry — so the sin and cos happen once rather than 36 times.
  attr_pose_set(i, q_from_rotvec(f * w.p4));

  pt_write(i, p, half, col * (0.35 + 0.65 * clamp(mag, 0.0, 1.0)));
"))

(define cam (make-camera))
(camera-distance-set! cam 3.4)
(define (frame! t) (orbit-camera! cam))

(run-wrangle-loop grid N kernel frame! cam
                  :canvas "vxs-gpu-canvas"
                  :params P
                  :scratch SCRATCH
                  :draw :cubes)
