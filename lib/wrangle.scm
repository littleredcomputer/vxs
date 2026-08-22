;;----------------------------------------------------------------------
;; The compute wrangle: a kernel that rewrites the point buffer in place
;;
;; Third harness. shadertoy is pixel -> colour, points is a draw, and this
;; is point -> point: the same buffer the renderer reads, rewritten on the
;; GPU instead of by Scheme.
;;
;; DELIBERATELY MINIMAL. The attribute vocabulary here is the smallest
;; thing that makes a compute kernel work — the point index, the time, and
;; get/set for position, size and colour. It is NOT the wrangle algebra:
;; there is no @P, no attribute binding, no notion of a node. Those are the
;; interesting design questions and they should be answered from several
;; real programs, not invented under the pressure of getting one to run.
;;
;; The body is WGSL for now, not the kernel language of lib/wgsl.scm.
;; Wiring the compiler in here is exactly the step that would force the
;; vocabulary decision, so it waits.
;;
;; What the body has in scope:
;;   i     u32, the point index (already bounds-checked)
;;   w     the uniform: w.time, w.count, w.seed
;;   pt_pos(i), pt_size(i), pt_colour(i)   read
;;   pt_write(i, pos, size, colour)        write
;;   the whole of lib/rng.wgsl and lib/stat.wgsl — random_normal,
;;   random_uniform, random_gamma, flip, and the logpdf_* family
;;   heat_colour(t), cool_colour(t) from lib/colour.wgsl — display ramps,
;;   not attributes
;;
;; rng_init is called for you, addressed by point index and w.seed, so
;; draws are reproducible per point and independent between points.
;;----------------------------------------------------------------------

;; The kernel compiler, for wgsl-declare! and define-gpu. Explicit rather
;; than relying on lib/gpu.scm having loaded it first: this file is loaded
;; directly by layer 18 and by anything that wants only the wrangle.
(load "lib/wgsl.scm")

;; For points-stride: the accessors below index the point buffer, so they
;; must agree with whatever fills it. Loading it here rather than trusting
;; the caller means the two cannot be loaded out of order.
(load "lib/points.scm")

;;--- what a Scheme kernel can call --------------------------------------
;; Signatures for the hand-written WGSL in lib/stat.wgsl and
;; lib/colour.wgsl. They are asserted rather than derived because nothing
;; can read a signature out of WGSL text — which is exactly the difference
;; between these and a define-gpu function, whose result type the compiler
;; works out for itself.
;;
;; Several of these could NOT be written in the kernel language: random_gamma
;; needs a rejection loop and erfc needs a polynomial evaluated in sequence.
;; That is the point of having two tiers rather than one. When the language
;; grows a curated `for-i`, some of them could move across, and no caller
;; would change.

(wgsl-declare! 'random-uniform     "random_uniform"     '(:f32 :f32)     :f32)
(wgsl-declare! 'random-normal      "random_normal"      '(:f32 :f32)     :f32)
(wgsl-declare! 'random-exponential "random_exponential" '(:f32)         :f32)
(wgsl-declare! 'random-gamma       "random_gamma"       '(:f32 :f32)     :f32)
(wgsl-declare! 'logpdf-normal      "logpdf_normal"      '(:f32 :f32 :f32) :f32)
(wgsl-declare! 'logpdf-uniform     "logpdf_uniform"     '(:f32 :f32 :f32) :f32)
(wgsl-declare! 'erfc               "erfc"               '(:f32)         :f32)
(wgsl-declare! 'inv-erf            "inv_erf"            '(:f32)         :f32)
(wgsl-declare! 'heat-colour        "heat_colour"        '(:f32)         :vec3f)
(wgsl-declare! 'cool-colour        "cool_colour"        '(:f32)         :vec3f)

;; What a wrangle kernel written in Scheme sees. `time` and `seed` are
;; struct fields on the uniform, which the environment handles directly:
;; a binding may carry the name to EMIT alongside the type, so the Scheme
;; name and the WGSL spelling need not match.
;; `count` and `seed` are u32 in the struct but :f32 to a Scheme kernel,
;; because WGSL has no implicit coercion and every use of them here is
;; arithmetic. The env carries the text to EMIT, so the conversion is
;; simply part of the spelling and no kernel has to know.
;;
;; seed is an INTEGER in the struct for a reason. It used to be an f32 that
;; the preamble converted with u32(w.seed), which silently aliases every
;; seed past 2^24 onto its neighbours — "a different seed" quietly meaning
;; "the same noise". A counter-based RNG addressed by a float is not
;; addressed at all.
(define wrangle-builtin-env
  '((time . (:f32 . "w.time"))
    (seed . (:f32 . "f32(w.seed)"))
    (count . (:f32 . "f32(w.count)"))
    (step . (:f32 . "f32(w.step)"))))

(define wrangle-env wrangle-builtin-env)

;;--- live parameters ----------------------------------------------------
;; (wrangle-params! '(sigma radius gain)) names the spare uniform slots.
;;
;; ONE declaration is the single source of truth for both sides: the kernel
;; writes `sigma` and it compiles to w.p0, and the host writes 'sigma and
;; it lands at float 0 of the parameter block. A positional (param 0) would
;; work too and would let the two drift apart silently, which is the whole
;; class of bug this project keeps finding.
(define wrangle-param-slots 8)
(define wrangle-param-names '())

(define (wrangle-params! names)
  (if (> (length names) wrangle-param-slots)
      (error "wrangle-params!: at most 8 parameters" (length names)))
  (set! wrangle-param-names names)
  ;; Rebuild rather than append: calling this twice should REPLACE the
  ;; declaration, not leave the previous names shadowing the new ones.
  (set! wrangle-env
        (append wrangle-builtin-env
                (let loop ((ns names) (i 0) (acc '()))
                  (if (null? ns)
                      (reverse acc)
                      (loop (cdr ns) (+ i 1)
                            (cons (cons (car ns)
                                        (cons :f32
                                              (string-append "w.p" (number->string i))))
                                  acc))))))
  wrangle-param-names)

;; The parameter block: eight floats the host owns and rewrites in place.
;;
;; A BYTES OBJECT, made once and mutated, rather than a fresh list per
;; frame. At sixty frames a second a list would allocate every frame, and
;; the demos this feeds run at zero objects per frame — which is the
;; property that lets them hold a frame budget at all.
(define (make-wrangle-params)
  (let ((b (make-bytes (* 4 wrangle-param-slots))))
    (bytes-seal! b)   ; the GPU will read it; it must not move
    b))
(define (wrangle-params-view p) (bytes-view p :f32))

(define (param-index name)
  (let loop ((ns wrangle-param-names) (i 0))
    (cond ((null? ns)
           (error "param: undeclared parameter — call wrangle-params! first" name))
          ((eq? (car ns) name) i)
          (else (loop (cdr ns) (+ i 1))))))

(define (param-set! v name value) (view-set! v (param-index name) value))
(define (param-ref v name) (view-ref v (param-index name)))

;; Kept as a name because tests and callers use it, but it is no longer an
;; independent number that could disagree — see points-stride-wgsl.
(define wrangle-stride points-stride)

(define wrangle-preamble
  (string-append
   "\n"
   "struct WU {\n"
   "  time : f32,\n"
   "  count : u32,\n"
   "  seed : u32,\n"
   "  step : u32,\n"
   ;; Eight general-purpose slots. A kernel constant baked into the SOURCE
   ;; means changing it recompiles the shader, so dragging a slider hitches
   ;; on every frame it moves. These live in the uniform, which is already
   ;; rewritten before every dispatch at no cost — the difference between a
   ;; knob you demonstrate and a knob you play.
   ;;
   ;; Written as eight named scalars and NOT array<f32, 8>: in the uniform
   ;; address space an array's stride is padded to 16 bytes, so the array
   ;; spelling would cost 128 bytes and index wrongly for anyone assuming
   ;; the floats are packed. Individual f32 members pack at 4-byte
   ;; alignment, so the struct is exactly 48 bytes.
   "  p0 : f32,\n  p1 : f32,\n  p2 : f32,\n  p3 : f32,\n"
   "  p4 : f32,\n  p5 : f32,\n  p6 : f32,\n  p7 : f32,\n"
   "};\n"
   "@group(0) @binding(0) var<uniform> w : WU;\n"
   "@group(0) @binding(1) var<storage, read_write> pts : array<f32>;\n"
   "\n"
   "fn pt_pos(i : u32) -> vec3<f32> {\n"
   "  let b = i * " points-stride-wgsl ";\n"
   "  return vec3<f32>(pts[b + 0u], pts[b + 1u], pts[b + 2u]);\n"
   "}\n"
   "fn pt_size(i : u32) -> f32 { return pts[i * " points-stride-wgsl " + 3u]; }\n"
   "fn pt_colour(i : u32) -> vec3<f32> {\n"
   "  let b = i * " points-stride-wgsl ";\n"
   "  return vec3<f32>(pts[b + 4u], pts[b + 5u], pts[b + 6u]);\n"
   "}\n"
   "fn pt_write(i : u32, p : vec3<f32>, size : f32, col : vec3<f32>) {\n"
   "  let b = i * " points-stride-wgsl ";\n"
   "  pts[b + 0u] = p.x;\n"
   "  pts[b + 1u] = p.y;\n"
   "  pts[b + 2u] = p.z;\n"
   "  pts[b + 3u] = size;\n"
   "  pts[b + 4u] = col.x;\n"
   "  pts[b + 5u] = col.y;\n"
   "  pts[b + 6u] = col.z;\n"
   "}\n"
   "\n"
   "@compute @workgroup_size(64)\n"
   "fn main(@builtin(global_invocation_id) gid : vec3<u32>) {\n"
   "  let i = gid.x;\n"
   "  // The dispatch rounds up to whole workgroups, so the tail runs with\n"
   "  // indices past the end. They must return, not clamp: clamping would\n"
   "  // have several invocations write the same point.\n"
   "  if (i >= w.count) { return; }\n"
   ;; THE SUBSTEP IS THE RNG STREAM. Running the kernel N times a frame
   ;; with one stream would replay the identical draws N times — the
   ;; positions move, because each step reads what the last one wrote, but
   ;; every random decision is the same one over and over. That is a
   ;; sampler that looks like it is working and is not.
   "  rng_init(i, w.seed, w.step);\n"))

;; (wrangle-wgsl body) -> complete compute shader source.
;; `body` is WGSL statements; see the header for what is in scope.
(define (wrangle-wgsl body)
  (let ((rng (embedded-source "rng.wgsl"))
        (stat (embedded-source "stat.wgsl"))
        (col (embedded-source "colour.wgsl")))
    (if (not (and rng stat col))
        (error 'wrangle
               "rng.wgsl, stat.wgsl or colour.wgsl is missing from the binary"))
    ;; Order matters: the libraries first, then anything define-gpu has
    ;; emitted (which may call them), then the kernel. WGSL wants a
    ;; function to appear before the code that calls it, which is also the
    ;; order a reader expects.
    (string-append rng "\n" stat "\n" col "\n"
                   (wgsl-definitions-source) "\n"
                   wrangle-preamble body "\n}\n")))
