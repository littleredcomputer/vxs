;;; ==========================================================
;;; Particle sun — a compute wrangle over 60k points. Drag to orbit,
;;; scroll to zoom.
;;; ==========================================================
;;; The host uploads the buffer ONCE and then never touches it. Each frame
;;; a compute dispatch rewrites every point in place, and the draw reads
;;; the same buffer.
;;;
;;; TEMPERATURE IS THE POINT'S OWN LOG-DENSITY. Each point's position is
;;; drawn from a normal, and logpdf_normal then asks how likely that
;;; position was — so the colour is not a stand-in for "near the middle",
;;; it IS the density of the distribution that produced the cloud. Dense
;;; core runs white-hot, the sparse outskirts fade through orange to a dim
;;; red. That is also the physical story for a star, which is why it reads
;;; as one.
;;;
;;; A sampler alone gives you particles; a sampler plus its log-density
;;; gives you weights. This picture is the second thing, used for colour
;;; rather than for inference — but it is the same quantity.
;;;
;;; SIZE IS ALMOST FLAT, on purpose. Letting temperature drive size as
;;; well as colour compounds under additive blending: the core gets more
;;; points AND bigger ones, saturates to white, and swallows the structure.
;;; Keeping size nearly constant lets DENSITY carry the information, which
;;; is what the distribution actually determines.
;;;
;;; Randomness is Threefry, addressed by POINT NUMBER — the same generator
;;; lib/threefry.scm runs on the host, checked against the same published
;;; vectors. A draw is a pure function of (index, seed) rather than a
;;; position in a stream, so re-running the kernel every frame reproduces
;;; the identical cloud instead of making it flicker.

(load "lib/gpu.scm")

(define N 60000)
(define cam (make-camera))
(define seed-buf (make-points N))   ; contents irrelevant: the GPU overwrites

(define kernel "
  // Three independent normals place the point; a gamma draw gives it an
  // orbital offset, so the cloud shears rather than rotating rigidly.
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

  // Temperature = joint log-density of this point's own position, under
  // the very distribution that placed it. The y term dominates because
  // its scale is smallest, which is what makes the disc read as a disc.
  let lp = logpdf_normal(x0, 0.0, 0.34)
         + logpdf_normal(y0, 0.0, 0.10)
         + logpdf_normal(z0, 0.0, 0.34);

// The ramp now has a cool end, so the band can span it. Measured against
  // the actual densities:
  //   mode  -> 0.96  white      1 sigma -> 0.84  amber
  //   1.5s  -> 0.61  red-orange 2 sigma -> 0.26  magenta
  //   3 sig -> 0.00  dim violet
  // A previous band peaked at orange because the ramp topped out at white
  // and the dense core accumulates toward white on its own. With a violet
  // tail underneath, the core can be allowed white per-point: most of the
  // visible area is now the long cool run, not the peak.
  let temp = smoothstep(-8.0, 3.0, lp);

  pt_write(i, p, 0.0030 + 0.0016 * temp, heat_colour(temp));
")

(define wrangle-src (wrangle-wgsl kernel))

(define (frame! t) (orbit-camera! cam))

(run-wrangle-loop seed-buf N wrangle-src frame! cam :canvas "vxs-gpu-canvas")
