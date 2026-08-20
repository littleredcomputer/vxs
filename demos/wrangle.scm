;;; ==========================================================
;;; Compute wrangle — the GPU rewrites the point buffer. Drag to orbit.
;;; ==========================================================
;;; The host uploads the buffer ONCE and then never touches it. Each frame
;;; a compute dispatch rewrites every point in place, and the draw reads
;;; the same buffer. Compare the obj/f and GC counters against the points
;;; preset, which rewrites all of it in Scheme every frame.
;;;
;;; The kernel body is WGSL, deliberately: wiring lib/wgsl.scm in here is
;;; what would force a decision about attribute syntax (@P and friends),
;;; and that decision wants evidence from several real programs first.
;;;
;;; Randomness is Threefry, addressed by POINT NUMBER — the same generator
;;; lib/threefry.scm runs on the host, checked against the same published
;;; vectors. Because a draw is a pure function of (index, seed) rather than
;;; a position in a stream, re-running the kernel every frame reproduces
;;; the identical cloud instead of making it flicker. That property is the
;;; whole reason for choosing a counter-based RNG, and this is the first
;;; place it is load-bearing.

(load "lib/gpu.scm")

(define N 60000)
(define cam (make-camera))
(define seed-buf (make-points N))   ; contents irrelevant: the GPU overwrites

(define kernel "
  // Three independent normals place the point; a gamma draw gives it an
  // orbital speed, so the cloud shears rather than rotating rigidly.
  let x0 = random_normal(0.0, 0.34);
  let y0 = random_normal(0.0, 0.10);
  let z0 = random_normal(0.0, 0.34);
  let g  = random_gamma(2.0, 2.6);

  let r  = length(vec2<f32>(x0, z0));
  let a  = w.time * (0.25 + 0.55 / (0.35 + r)) + g;
  let ca = cos(a);
  let sa = sin(a);

  let p = vec3<f32>(x0 * ca - z0 * sa,
                    y0 * (1.0 + 0.25 * sin(w.time * 0.6 + g)),
                    x0 * sa + z0 * ca);

  let heat = clamp(1.0 - r * 1.6, 0.0, 1.0);
  let col = vec3<f32>(0.35 + 0.65 * heat,
                      0.30 + 0.35 * fract(g),
                      0.75 - 0.35 * heat);

  pt_write(i, p, 0.0035 + 0.0075 * heat, col);
")

(define wrangle-src (wrangle-wgsl kernel))

(define (frame! t) (orbit-camera! cam))

(run-wrangle-loop seed-buf N wrangle-src frame! cam "vxs-gpu-canvas")
