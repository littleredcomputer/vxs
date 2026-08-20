;;----------------------------------------------------------------------
;; Layer 18: the compute wrangle (lib/wrangle.scm, lib/rng.wgsl, lib/stat.wgsl)
;;
;; Third harness: shadertoy is pixel -> colour, points is a draw, this is
;; point -> point — the same buffer the renderer reads, rewritten on the
;; GPU rather than by Scheme.
;;
;; NOTHING HERE RUNS WGSL. There is no GPU in the test process, so these are
;; structural assertions about assembled text, and they are honest about
;; that: they can catch a stride that stopped matching, a workgroup size
;; that stopped matching its dispatch, or a library that failed to be
;; included. They cannot catch a shader that compiles and computes the
;; wrong thing. That still needs eyes on a screen.
;;
;; What makes the structural tests worth writing anyway is that every one
;; of them guards a COUPLING between two files that cannot see each other,
;; and every such mismatch is silent rather than loud.
;;----------------------------------------------------------------------

(load "testcases/test_framework.scm")
(load "lib/points.scm")
(load "lib/wrangle.scm")
(load "lib/gpu.scm")

(test-suite "18_wrangle: GPU compute over the point buffer")

(define (string-contains? haystack needle)
  (let ((hn (string-length haystack)) (nn (string-length needle)))
    (let loop ((i 0))
      (cond ((> (+ i nn) hn) #f)
            ((string=? (substring haystack i (+ i nn)) needle) #t)
            (else (loop (+ i 1)))))))

(define src (wrangle-wgsl "  pt_write(i, pt_pos(i), 0.01, vec3<f32>(1.0, 1.0, 1.0));"))

;;--- the libraries are reachable and included ---------------------------

(assert-true "rng.wgsl is embedded"  (string? (embedded-source "rng.wgsl")))
(assert-true "stat.wgsl is embedded" (string? (embedded-source "stat.wgsl")))
(assert-equal "a missing source is #f, not an error" #f
              (embedded-source "no-such-file.wgsl"))
(assert-true "embedded-source ignores a leading path"
             (string? (embedded-source "lib/rng.wgsl")))

(assert-true "the assembled kernel carries the RNG"
             (string-contains? src "fn threefry4x32(ctr: vec4u, key: vec4u) -> vec4u {"))
(assert-true "and the stat library"
             (string-contains? src "fn random_normal(loc: f32, scale: f32) -> f32 {"))
(assert-true "and the body it was given"
             (string-contains? src "pt_write(i, pt_pos(i), 0.01"))

;;--- the RNG was actually rewired ---------------------------------------
;; stat.wgsl was written against a pcg3d STREAM. A stream makes a draw
;; depend on how many draws came before it, so a point's randomness would
;; depend on evaluation order — meaningless across thousands of invocations
;; in unspecified order, and impossible to reproduce on the host. Threefry
;; is a pure function of (counter, key) instead.

(assert-true "random_uniform draws from the Threefry block"
             (string-contains? src "let a: f32 = rng_unit();"))
;; Matched with a paren: the function and any call to it must be gone,
;; while the comments EXPLAINING why it was replaced should stay. A test
;; that forbade the word outright would forbid the explanation too.
(assert-false "no pcg3d function or call survives"
              (string-contains? src "pcg3d("))
(assert-false "and so is its mutable seed"
              (string-contains? src "seed = pcg3d(seed)"))
(assert-true "draws are addressed by point index"
             (string-contains? src "rng_init(i, u32(w.seed), 0u);"))

;; The rotation constants are the ones the host uses. If these ever
;; disagreed, GPU and host randomness would silently diverge — and the
;; nine published vectors in layer 13 would no longer describe this code.
(assert-true "the same 8 rotation pairs as lib/threefry.scm"
             (string-contains? src "10u, 26u, 11u, 21u, 13u, 27u, 23u,  5u"))
(assert-true "the same Skein parity constant"
             (string-contains? src "0x1BD11BDAu"))
(assert-true "the same 13 rounds"
             (string-contains? src "r < 13u"))

;;--- couplings with the rest of the system ------------------------------

;; The wrangle writes the buffer the renderer reads, so the two strides
;; must agree. They live in different files and nothing but this says so.
(assert-equal "the wrangle stride matches the point stride"
              points-stride wrangle-stride)
(assert-true "the accessors stride by 7"
             (and (string-contains? src "let b = i * 7u;")
                  (string-contains? src "return pts[i * 7u + 3u];")))
(assert-true "colour sits where the renderer looks for it"
             (string-contains? src "pts[b + 6u] = col.z;"))

;; Workgroup size here must match the ceil(count / 64) in
;; js_gpu_wrangle's dispatchWorkgroups. Too small a dispatch silently
;; leaves the tail of the buffer un-updated.
(assert-true "workgroup size is 64, matching the dispatch"
             (string-contains? src "@compute @workgroup_size(64)"))
(assert-true "the entry point is main"
             (string-contains? src "fn main(@builtin(global_invocation_id)"))

;; The tail of the last workgroup runs with indices past the end. It must
;; RETURN — clamping would make several invocations write the same point.
(assert-true "out-of-range invocations return"
             (string-contains? src "if (i >= u32(w.count)) { return; }"))

;;--- the storage binding is writable ------------------------------------
;; The renderer binds the same buffer read-only; the wrangle must not.
(assert-true "the wrangle binds storage read_write"
             (string-contains? src "var<storage, read_write> pts : array<f32>;"))
(assert-true "the renderer binds the same buffer read-only"
             (string-contains? points-wgsl "var<storage, read> pts : array<f32>;"))

;;--- what a kernel body has in scope -------------------------------------

(assert-true "position accessor"  (string-contains? src "fn pt_pos(i : u32)"))
(assert-true "size accessor"      (string-contains? src "fn pt_size(i : u32)"))
(assert-true "colour accessor"    (string-contains? src "fn pt_colour(i : u32)"))
(assert-true "writer"             (string-contains? src "fn pt_write(i : u32,"))
(assert-true "the uniform carries time, count and seed"
             (string-contains? src "struct WU {\n  time : f32,\n  count : f32,\n  seed : f32,"))

;; The distributions the body can reach. Named individually because the
;; point of welding this library in was to have them.
(assert-true "random_uniform"    (string-contains? src "fn random_uniform("))
(assert-true "random_normal"     (string-contains? src "fn random_normal("))
(assert-true "random_exponential" (string-contains? src "fn random_exponential("))
(assert-true "random_gamma"      (string-contains? src "fn random_gamma("))
(assert-true "flip"              (string-contains? src "fn flip("))
;; The log-densities are the half that turns particles into inference:
;; a sampler gives you a cloud, a sampler plus its log-density gives you
;; importance weights.
(assert-true "logpdf_normal"     (string-contains? src "fn logpdf_normal("))
(assert-true "logpdf_uniform"    (string-contains? src "fn logpdf_uniform("))
(assert-true "logpdf_flip"       (string-contains? src "fn logpdf_flip("))

;;--- colour ramps --------------------------------------------------------
;; Display helpers rather than attributes: a wrangle decides what a point
;; IS, these decide how to show it. Their own file so watch mode reloads
;; them on save, since a ramp is the thing you tweak most.
(assert-true "colour.wgsl is embedded" (string? (embedded-source "colour.wgsl")))
(assert-true "heat_colour is in scope" (string-contains? src "fn heat_colour(t : f32)"))
(assert-true "cool_colour is in scope" (string-contains? src "fn cool_colour(t : f32)"))
;; The cool end is dim, never black: under additive blending a black point
;; is invisible, so the outskirts would not fade, they would vanish and
;; take the cloud's silhouette with them.
(assert-true "the heat ramp has a cool end to fall off toward"
             (string-contains? src "vec3<f32>(0.08, 0.04, 0.22)"))
(assert-true "and a white-hot top"
             (string-contains? src "vec3<f32>(1.00, 0.98, 0.88)"))
;; The min against 3 matters at exactly t = 1: without it the last segment
;; degenerates to its own lower endpoint and the hottest points come out
;; amber instead of white.
(assert-true "the last ramp segment does not degenerate at t = 1"
             (string-contains? src "let i = min(floor(s), 3.0);"))

;;--- the driver surface exists ------------------------------------------
;; Defining these needs no GPU — only CALLING them does — so a native test
;; can check they are all still here. That is worth a test because one of
;; them was silently deleted: an edit that replaced "from this comment to
;; the end of the file" took run-wrangle-loop with it, and nothing noticed
;; until a preset failed in the browser with an unbound variable. Every
;; entry point a demo can name belongs on this list.

(assert-true "run-kernel-loop"  (procedure? run-kernel-loop))
(assert-true "run-points-loop"  (procedure? run-points-loop))
(assert-true "run-wrangle-loop" (procedure? run-wrangle-loop))
(assert-true "make-orbiter"     (procedure? make-orbiter))
(assert-true "orbit-camera!"    (procedure? orbit-camera!))
(assert-true "make-camera"      (procedure? make-camera))
(assert-true "wrangle-wgsl"     (procedure? wrangle-wgsl))
(assert-true "make-points"      (procedure? make-points))
(assert-true "points-view"      (procedure? points-view))
(assert-true "point-set!"       (procedure? point-set!))
(assert-true "shadertoy"        (procedure? shadertoy))
(assert-true "points-wgsl is a string" (string? points-wgsl))

;;--- callable functions: declared and defined ---------------------------
;; Two kinds, one table, and a caller cannot tell them apart. A DECLARED
;; function is hand-written WGSL whose signature is asserted here, because
;; nothing can read a signature out of WGSL text. A DEFINED one is written
;; in the kernel language, and its result type is DERIVED by the same
;; checker that checks everything else — which is what makes the signature
;; honest rather than an assertion that could drift from the body.

(assert-equal "the stat library is declared"
              '(random-normal "random_normal" (:f32 :f32) :f32)
              (wgsl-signature 'random-normal))
(assert-equal "so are the colour ramps"
              '(heat-colour "heat_colour" (:f32) :vec3f)
              (wgsl-signature 'heat-colour))

(define-gpu (test-falloff (r :f32) (k :f32))
  (/ 1.0 (+ 1.0 (* k r r))))

(assert-equal "define-gpu derives the result type from the body"
              '(test-falloff "test_falloff" (:f32 :f32) :f32)
              (wgsl-signature 'test-falloff))

;; A defined function may call a declared one, and the result type follows
;; through. This is the composition that makes a library possible at all.
(define-gpu (test-ember (r :f32))
  (heat-colour (test-falloff r 2.5)))
(assert-equal "a defined function composing a declared one"
              :vec3f (cadddr (wgsl-signature 'test-ember)))

(assert-true "it emits a real WGSL function"
             (string-contains? (wgsl-definitions-source)
                               "fn test_falloff(r : f32, k : f32) -> f32 {"))
(assert-true "hyphens become underscores"
             (string-contains? (wgsl-definitions-source) "fn test_ember(r : f32)"))
(assert-true "the emitted body returns the compiled expression"
             (string-contains? (wgsl-definitions-source)
                               "return (1.0 / (1.0 + ((k * r) * r)));"))

;; Call sites are checked against the derived signature.
(define E7 '((v . :vec3f) (s . :f32)))
(define (rejects-call? e) (guard (c (#t #t)) (wgsl-code e E7) #f))
(assert-equal "a well-typed call" :f32 (wgsl-type '(test-falloff s s) E7))
(assert-true "wrong argument type is rejected" (rejects-call? '(test-falloff v s)))
(assert-true "too few arguments is rejected"   (rejects-call? '(test-falloff s)))
(assert-true "too many arguments is rejected"  (rejects-call? '(test-falloff s s s)))
(assert-true "an undeclared function is still unknown"
             (rejects-call? '(no-such-gpu-fn s)))

;; A parameter list must be (name type) with a type this language has.
(assert-true "a bad parameter spec is rejected"
             (guard (c (#t #t)) (wgsl-define-fn! 'bad '((r)) '1.0) #f))
(assert-true "an unknown parameter type is rejected"
             (guard (c (#t #t)) (wgsl-define-fn! 'bad '((r i32)) '1.0) #f))

;; Redefining replaces in place rather than appending — watch mode re-runs
;; a file on every save, so a table that only grew would accumulate a stale
;; entry per keystroke.
(define before-count (string-length (wgsl-definitions-source)))
(define-gpu (test-falloff (r :f32) (k :f32))
  (/ 1.0 (+ 1.0 (* k r r))))
(assert-equal "redefinition does not append a duplicate"
              before-count (string-length (wgsl-definitions-source)))

;; The uniform reaches a Scheme kernel through the environment: a binding
;; may carry the name to EMIT alongside its type, so `time` in Scheme is
;; `w.time` in WGSL without the kernel language knowing what a struct is.
(assert-equal "time is bound to the uniform field"
              "w.time" (wgsl-code 'time wrangle-env))
(assert-equal "and carries its type" :f32 (wgsl-type 'time wrangle-env))

(suite-summary)
