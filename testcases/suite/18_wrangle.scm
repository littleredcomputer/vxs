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
;; rng_unit must never return exactly 0. It does so for the 512 smallest
;; words out of 2^32 — probability 2^-23, about ten draws a second at the
;; rates a real kernel runs at — and random_uniform(-1, 1) then returns
;; exactly -1, whereupon inv_erf(-1) is inv_erfc(2), pp is 0, log(0) is
;; -inf, and the rational term is inf/inf. random_normal returns NaN and
;; the element is poisoned permanently with no diagnostic.
(assert-true "rng_unit is clamped away from zero"
             (string-contains? src "max(bitcast<f32>((rng_u32() >> 9u) | 1065353216u) - 1.0, 5.9604645e-8)"))

;; NO u32() CONVERSION HERE. The seed is a u32 in the struct because it
;; used to be an f32 the preamble converted, and that aliases every seed
;; past 2^24 onto its neighbours — "a different seed" quietly meaning "the
;; same noise". A counter-based RNG addressed by a float is not addressed.
(assert-true "draws are addressed by point index, from an integer seed"
             (string-contains? src "rng_init(i, w.seed, w.step);"))

;; THE SUBSTEP IS THE STREAM. Running the kernel N times a frame with one
;; stream replays the identical draws N times: the positions still move,
;; because each step reads what the last one wrote, but every random
;; decision repeats. That is a sampler that looks like it works.
(assert-equal "the substep index is readable from a kernel"
              "f32(w.step)" (wgsl-code 'step wrangle-env))

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
             (string-contains? src "if (i >= w.count) { return; }"))

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
             (string-contains? src "struct WU {\n  time : f32,\n  count : u32,\n  seed : u32,\n  step : u32,"))

;; Sixteen spare slots, and named scalars rather than array<f32, 16>: in
;; the uniform address space an array's stride is padded to 16 bytes, so
;; the array spelling would cost 256 bytes and index wrongly for anyone who
;; assumed the floats were packed. This way the members occupy 116 bytes,
;; which WGSL rounds to 128 — and a substep's slice costs 256 whatever the
;; struct does, since it is addressed by dynamic offset.
(assert-true "and sixteen live parameter slots"
             (string-contains? src "  p0 : f32,\n  p1 : f32,"))
(assert-true "through to the sixteenth"
             (string-contains? src "  p15 : f32,\n};"))
(assert-true "not an array, whose uniform stride would be 16 bytes each"
             (not (string-contains? src "array<f32, 16>")))
(assert-true "and eight integer slots, ahead of the floats"
             (string-contains? src "  i7 : u32,\n  p0 : f32,"))

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

;;--- live parameters ----------------------------------------------------
;; A kernel constant baked into the shader SOURCE means changing it
;; recompiles, so a slider hitches on every frame it moves. These slots
;; live in the uniform, which is rewritten before every dispatch anyway.
;;
;; The declaration is the single source of truth for both sides: the kernel
;; writes `sigma` and gets w.p0, the host writes 'sigma and hits float 0.
;; A positional (param 0) would let the two drift apart silently.

(wrangle-params! '(sigma radius gain))
(assert-equal "a declared parameter emits its slot" "w.p0" (wgsl-code 'sigma wrangle-env))
(assert-equal "in declaration order" "w.p2" (wgsl-code 'gain wrangle-env))
(assert-equal "and is an f32 to the kernel" :f32 (wgsl-type 'gain wrangle-env))
(assert-equal "parameters compose into expressions"
              "(w.p0 * w.time)" (wgsl-code '(* sigma time) wrangle-env))

;; count and seed are u32 in the struct but :f32 here, because WGSL has no
;; implicit coercion and every use of them in a kernel is arithmetic. The
;; env carries the text to emit, so the conversion is part of the spelling.
(assert-equal "seed reads as a float in a kernel"
              "f32(w.seed)" (wgsl-code 'seed wrangle-env))
(assert-equal "and so does count"
              "f32(w.count)" (wgsl-code 'count wrangle-env))

;; Redeclaring REPLACES. Appending instead would leave the old names
;; resolving to slots the host is no longer writing.
(wrangle-params! '(alpha))
(assert-equal "redeclaring rebinds the first slot" "w.p0" (wgsl-code 'alpha wrangle-env))
(assert-equal "and drops the previous names" #f (assq 'sigma wrangle-env))
(assert-equal "while the built-ins survive" "w.time" (wgsl-code 'time wrangle-env))

;; The block itself: sixteen floats, made once and written in place, because
;; a fresh list per frame would allocate sixty times a second in a demo
;; that otherwise runs at zero objects per frame.
(wrangle-params! '(sigma radius gain))
(define PB (make-wrangle-params))
(define PV (wrangle-params-view PB))
(assert-equal "the block is sixteen floats, a flag word and eight ints"
              100 (bytes-length PB))
(assert-equal "unset slots read zero" 0.0 (param-ref PV 'gain))
(param-set! PV 'gain 2.5)
(param-set! PV 'sigma 0.25)
(assert-equal "a parameter round-trips by name" 2.5 (param-ref PV 'gain))
(assert-equal "and does not disturb its neighbours" 0.25 (param-ref PV 'sigma))
(assert-equal "or the ones between" 0.0 (param-ref PV 'radius))
(assert-equal "writing by name lands in the declared slot" 0.25 (view-ref (car PV) 0))
(assert-equal "third name, third float" 2.5 (view-ref (car PV) 2))

(assert-equal "an undeclared name is an error, not slot zero"
              'raised (guard (e (#t 'raised)) (param-set! PV 'nonesuch 1.0)))
(assert-equal "more than sixteen float parameters is an error"
              'raised (guard (e (#t 'raised))
                        (wrangle-params! '(a b c d e f g h i j k l m n o p q))))

;;--- typed parameters ---------------------------------------------------
;; The type decides which slot a parameter lands in: :f32 takes one of
;; sixteen float slots, :u32 an honest integer slot, :flag one bit of a word.
;; ONE declarator rather than three, and a bare symbol still means :f32.
;;
;; :u32 and :flag are not tidiness. An f32 has 24 mantissa bits, so an
;; integer or a bitfield carried in a float slot works perfectly to bit 23
;; and then silently drops the rest — which survives every test written
;; early. The seed had exactly this bug.

(wrangle-params! '(sigma (mode :u32) (flat :flag) (trails :flag)))
(assert-equal "a float parameter still takes a p slot"
              "w.p0" (wgsl-code 'sigma wrangle-env))
(assert-equal "an integer parameter takes an i slot"
              "w.i0" (wgsl-code 'mode wrangle-env))
(assert-equal "and keeps its type, so it cannot be used as a quantity by accident"
              :u32 (wgsl-type 'mode wrangle-env))
;; A flag reads as :bool, so it drops into (if flat a b) with no new
;; machinery — and the mask is a shift, not a decimal, because bit 31 of a
;; decimal literal would have to survive a flonum on the way out.
(assert-equal "a flag reads as a bool"
              :bool (wgsl-type 'flat wrangle-env))
(assert-equal "and tests its bit by shifting"
              "((w.flags & (1u << 0u)) != 0u)" (wgsl-code 'flat wrangle-env))
(assert-equal "the second flag takes the second bit"
              "((w.flags & (1u << 1u)) != 0u)" (wgsl-code 'trails wrangle-env))
(assert-true "and flags also get a helper for WGSL-text bodies"
             (string-contains? (wrangle-flag-preamble)
               "fn flag_trails() -> bool { return (w.flags & (1u << 1u)) != 0u; }"))
(assert-equal "an unknown parameter type is refused"
              'raised (guard (e (#t 'raised)) (wrangle-params! '((x :vec3f)))))
(assert-equal "at most eight integer slots"
              'raised (guard (e (#t 'raised))
                        (wrangle-params! '((a :u32) (b :u32) (c :u32) (d :u32)
                                           (e :u32) (f :u32) (g :u32) (h :u32)
                                           (i :u32)))))

(wrangle-params! '(sigma (mode :u32) (flat :flag) (trails :flag)))
(define TB (make-wrangle-params))
(define TV (wrangle-params-view TB))
(param-set! TV 'sigma 0.75)
(param-set! TV 'mode 16777217)
(param-set! TV 'trails #t)
(assert-equal "a float round-trips" 0.75 (param-ref TV 'sigma))
;; 2^24+1 is the first integer an f32 cannot represent. This is the whole
;; reason :u32 exists rather than everything being a float.
(assert-equal "an integer parameter survives past 2^24 exactly"
              16777217 (param-ref TV 'mode))
(assert-equal "a set flag reads true" #t (param-ref TV 'trails))
(assert-equal "an unset flag reads false" #f (param-ref TV 'flat))
(param-set! TV 'flat #t)
(assert-equal "setting one flag does not disturb its neighbour"
              '(#t #t) (list (param-ref TV 'flat) (param-ref TV 'trails)))
(param-set! TV 'trails #f)
(assert-equal "and clearing one leaves the other alone"
              '(#t #f) (list (param-ref TV 'flat) (param-ref TV 'trails)))
(assert-equal "the integer slot is untouched by flag writes"
              16777217 (param-ref TV 'mode))

;; 32 flags, so bit 31 is exercised — the bit a 24-bit mantissa could not
;; have held, and the one a decimal mask would have emitted as a flonum.
(wrangle-params! (let loop ((i 0) (acc '()))
                   (if (= i 32) (reverse acc)
                       (loop (+ i 1)
                             (cons (list (string->symbol
                                          (string-append "b" (number->string i)))
                                         :flag)
                                   acc)))))
(define FB (make-wrangle-params))
(define FV (wrangle-params-view FB))
(param-set! FV 'b31 #t)
(param-set! FV 'b0 #t)
(assert-equal "the highest bit sets" #t (param-ref FV 'b31))
(assert-equal "without disturbing the lowest" #t (param-ref FV 'b0))
(param-set! FV 'b31 #f)
(assert-equal "and clears again" '(#f #t) (list (param-ref FV 'b31) (param-ref FV 'b0)))
(assert-equal "a 33rd flag is refused"
              'raised (guard (e (#t 'raised))
                        (wrangle-params! (let loop ((i 0) (acc '()))
                          (if (= i 33) (reverse acc)
                              (loop (+ i 1)
                                    (cons (list (string->symbol
                                                 (string-append "c" (number->string i)))
                                                :flag) acc)))))))
(wrangle-params! '(sigma radius gain))   ; leave the env as we found it

;;--- the accessors derive their stride from lib/points.scm ---------------
(assert-true "pt_pos indexes by the shared stride"
             (string-contains? src (string-append "let b = i * " points-stride-wgsl ";")))
(assert-true "and so does pt_size"
             (string-contains? src (string-append "pts[i * " points-stride-wgsl " + 3u]")))

;;--- named scratch attributes -------------------------------------------
;; State the simulation needs and the renderer must not see. A SECOND
;; buffer rather than a wider point stride: the renderer is the hot path,
;; and widening the stride would make the vertex shader fetch every scratch
;; float of every point of every frame to use none of them.

(assert-equal "with nothing declared, the shader is unchanged"
              "" (begin (scratch-attributes! '()) (scratch-preamble)))
(assert-equal "and no binding 2 appears" #f
              (string-contains? (wrangle-wgsl "") "@binding(2)"))

(assert-equal "the stride is the sum of the component widths"
              5 (scratch-attributes! '((weight :f32) (ancestor :u32) (velocity :vec3f))))
(assert-equal "offsets follow declaration order" 0 (scratch-offset 'weight))
(assert-equal "one float for a u32" 1 (scratch-offset 'ancestor))
(assert-equal "three for a vec3f" 2 (scratch-offset 'velocity))
(assert-equal "an unknown type is refused"
              'raised (guard (e (#t 'raised)) (scratch-attributes! '((x :mat4)))))
(assert-equal "so is a malformed spec"
              'raised (guard (e (#t 'raised)) (scratch-attributes! '(weight))))

;; A kernel says `weight` and means THIS point's weight, the way VEX means
;; @weight. Every invocation owns exactly one index, so the name can bind
;; straight to the call.
(scratch-attributes! '((weight :f32) (ancestor :u32) (velocity :vec3f)))
(assert-equal "a kernel reads an attribute by name"
              "attr_weight(i)" (wgsl-code 'weight wrangle-env))
(assert-equal "a vec3f attribute keeps its type"
              :vec3f (wgsl-type 'velocity wrangle-env))
;; A u32 attribute reads as :f32 in the kernel language for the same reason
;; seed does: WGSL has no implicit coercion and kernel use is arithmetic.
(assert-equal "a u32 attribute is converted for arithmetic"
              "f32(attr_ancestor(i))" (wgsl-code 'ancestor wrangle-env))

(define spre (scratch-preamble))
(assert-true "the scratch buffer binds at 2, read_write"
             (string-contains? spre
               "@group(0) @binding(2) var<storage, read_write> scratch : array<f32>;"))
(assert-true "an f32 accessor indexes by the scratch stride"
             (string-contains? spre "fn attr_weight(i : u32) -> f32 { return scratch[i * 5u + 0u]; }"))
(assert-true "and has a setter"
             (string-contains? spre "fn attr_weight_set(i : u32, v : f32) { scratch[i * 5u + 0u] = v; }"))
;; BITCAST, not a numeric conversion. An ancestor index must survive the
;; round trip exactly, and f32 cannot hold every u32 — the same failure the
;; seed had before it became an integer.
(assert-true "a u32 attribute bitcasts rather than converts"
             (string-contains? spre "return bitcast<u32>(scratch[i * 5u + 1u]);"))
(assert-true "in both directions"
             (string-contains? spre "scratch[i * 5u + 1u] = bitcast<f32>(v);"))
(assert-true "a vec3f is three flat floats, not a vec3 in the array"
             (string-contains? spre "vec3<f32>(scratch[b], scratch[b + 1u], scratch[b + 2u])"))

;;--- the host side ------------------------------------------------------
(define SB (make-scratch 4))
(define SV (scratch-view SB))
(assert-equal "the block is stride * count floats" 80 (bytes-length SB))
(scratch-set! SV 0 'weight 0.25)
(scratch-set! SV 0 'velocity 1.0 2.0 3.0)
(scratch-set! SV 3 'weight 0.5)
(assert-equal "an attribute round-trips by name" 0.25 (scratch-ref SV 0 'weight))
(assert-equal "a vec3f round-trips" '(1.0 2.0 3.0) (scratch-ref SV 0 'velocity))
(assert-equal "elements do not disturb each other" 0.5 (scratch-ref SV 3 'weight))
(assert-equal "unset reads zero" 0.0 (scratch-ref SV 1 'weight))

;; The reason :u32 exists at all. 2^24+1 is the first integer an f32 cannot
;; represent; stored as a float it comes back as its neighbour.
(scratch-set! SV 2 'ancestor 16777217)
(assert-equal "a u32 attribute survives past 2^24 exactly"
              16777217 (scratch-ref SV 2 'ancestor))

(assert-equal "an undeclared attribute is an error, not offset zero"
              'raised (guard (e (#t 'raised)) (scratch-ref SV 0 'nonesuch)))

;; Declaring again REPLACES. Appending would leave every previous name
;; resolving to a slot nobody writes any more.
(scratch-attributes! '((age :f32)))
(assert-equal "redeclaring rebinds from zero" 0 (scratch-offset 'age))
(assert-equal "and drops the previous names" #f (assq 'weight wrangle-env))
(wrangle-params! '(sigma))
(assert-equal "parameters and attributes coexist"
              "w.p0" (wgsl-code 'sigma wrangle-env))
(assert-equal "and neither declarator clears the other"
              "attr_age(i)" (wgsl-code 'age wrangle-env))

;;--- :quat, the stock orientation type -----------------------------------
;; Orientation turns up often enough that it should not have to be spelled
;; as four loose floats each time, and naming it is what lets a renderer
;; recognise it: an attribute called `pose` of type :quat is the
;; convention the cube shader looks for.

(scratch-attributes! '((charge :f32) (pose :quat)))
(assert-equal "a quaternion is four floats" 5 scratch-stride)
(assert-equal "and follows what came before" 1 (scratch-offset 'pose))
;; The storage type carries the intent; the type system carries the shape.
(assert-equal "it reads as a vec4f in the kernel language"
              :vec4f (wgsl-type 'pose wrangle-env))
(assert-equal "so it goes straight into q-rot"
              "q_rot(attr_pose(i), v)"
              (wgsl-code '(q-rot pose v) (cons (cons 'v :vec3f) wrangle-env)))
;; The rotation vector is the right form to GENERATE from — singularity
;; free at zero, where the axis stops meaning anything exactly as the angle
;; vanishes. It is the wrong form to STORE, because the renderer applies
;; the rotation once per vertex and would pay a sin and a cos each time.
(assert-equal "and a rotation vector converts to one"
              :vec4f (wgsl-type '(q-from-rotvec f) (cons (cons 'f :vec3f) wrangle-env)))

(define QB (make-scratch 2))
(define QV (scratch-view QB))
(scratch-set! QV 1 'pose 0.0 0.0 0.7071 0.7071)
(assert-equal "a quaternion round-trips as four values"
              4 (length (scratch-ref QV 1 'pose)))
;; Compared with a tolerance: the value makes a round trip through an f32,
;; and 0.7071 is not one of the doubles an f32 can hold exactly.
(assert-true "scalar last, and in order"
             (< (abs (- 0.7071 (list-ref (scratch-ref QV 1 'pose) 3))) 1e-6))
(assert-true "and the vector part with it"
             (< (abs (- 0.7071 (list-ref (scratch-ref QV 1 'pose) 2))) 1e-6))
(assert-equal "without disturbing its neighbour"
              '(0.0 0.0 0.0 0.0) (scratch-ref QV 0 'pose))
(assert-equal "or the attribute beside it" 0.0 (scratch-ref QV 1 'charge))
(scratch-attributes! '())

;;--- shared read-only data ----------------------------------------------
;; Data every element READS, as against scratch, which each element OWNS.
;; Scratch cannot hold it: scratch is addressed scratch[i * stride + off],
;; so it is per-element by construction. The parameter block is eight
;; floats. And anything changing per frame cannot be baked into source.

(assert-equal "with nothing declared, no binding 3 appears"
              "" (begin (shared-layout! '()) (shared-preamble)))

(assert-equal "the layout totals its regions"
              89 (shared-layout! '((walls 48) (obs 41))))
(assert-equal "regions are laid out in order" 0 (shared-offset 'walls))
(assert-equal "the next follows the first" 48 (shared-offset 'obs))
(assert-equal "and each keeps its length" 41 (shared-size 'obs))
(assert-equal "a malformed region is refused"
              'raised (guard (e (#t 'raised)) (shared-layout! '((walls)))))
(assert-equal "so is a zero-length one"
              'raised (guard (e (#t 'raised)) (shared-layout! '((walls 0)))))
(shared-layout! '((walls 48) (obs 41)))

(define shpre (shared-preamble))
;; READ-ONLY, and named sdata rather than `shared` — `shared` is a reserved
;; word in WGSL and a binding using it will not compile.
(assert-true "it binds at 3, read-only"
             (string-contains? shpre
               "@group(0) @binding(3) var<storage, read> sdata : array<f32>;"))
(assert-false "and does not use the reserved word `shared`"
              (string-contains? shpre "> shared :"))
;; Accessors carry the offset, so no kernel ever writes one by hand — an
;; offset computed in two places is one that eventually disagrees.
(assert-true "each region gets an accessor carrying its offset"
             (string-contains? shpre
               "fn shared_obs(k : u32) -> f32 { return sdata[48u + k]; }"))
(assert-true "the first region starts at zero"
             (string-contains? shpre
               "fn shared_walls(k : u32) -> f32 { return sdata[0u + k]; }"))
;; And the signature is registered, so the kernel language can call it.
(assert-equal "a kernel may call a region accessor"
              :f32 (wgsl-type '(shared-obs k) (list (cons 'k :u32))))

(define SHB (make-shared))
(define SHV (shared-view SHB))
(assert-equal "the block is the declared length" (* 89 4) (bytes-length SHB))
(shared-set! SHV 'obs 0 2.5)
(shared-set! SHV 'walls 0 1.5)
(assert-equal "a region round-trips by name" 2.5 (shared-ref SHV 'obs 0))
(assert-equal "and regions do not overlap" 1.5 (shared-ref SHV 'walls 0))
(assert-equal "an index past a region's end is refused, not silently next door"
              'raised (guard (e (#t 'raised)) (shared-ref SHV 'obs 41)))
(assert-equal "and an undeclared region is an error"
              'raised (guard (e (#t 'raised)) (shared-ref SHV 'nonesuch 0)))

;;--- gradient noise ------------------------------------------------------
;; Perlin needs a pseudo-random gradient at every integer lattice point,
;; and the classical route is a permutation table or a hand-rolled integer
;; hash — both invented, neither checkable, and a bad one shows up as
;; visible lattice structure. A counter-based RNG is addressed BY INDEX,
;; and a lattice point IS an index, so the gradient is one Threefry block
;; with the coordinates as its counter.

(assert-true "the noise library is embedded"
             (if (embedded-source "noise.wgsl") #t #f))
(assert-equal "perlin3 is callable from a kernel and returns a scalar"
              :f32 (wgsl-type '(perlin3 p s) (list (cons 'p :vec3f) (cons 's :u32))))
(assert-equal "perlin3v gives a vector field"
              :vec3f (wgsl-type '(perlin3v p s) (list (cons 'p :vec3f) (cons 's :u32))))
(assert-equal "and the seed must be a u32, not a float"
              'raised (guard (e (#t 'raised))
                        (wgsl-type '(perlin3 p s)
                                   (list (cons 'p :vec3f) (cons 's :f32)))))
(define nsrc (embedded-source "noise.wgsl"))
;; It must not touch the streaming RNG state: a noise call that went
;; through rng_init would silently consume a kernel's draws and shift every
;; random decision downstream of it.
;; Checks for a CALL and for the state itself, not for the bare name —
;; noise.wgsl's own comment explains why it avoids rng_init, and an earlier
;; version of this assertion matched that prose and failed.
(assert-false "noise never calls into the streaming generator"
              (string-contains? nsrc "rng_init("))
(assert-false "nor touches its private state"
              (or (string-contains? nsrc "rng_ctr")
                  (string-contains? nsrc "rng_key")))
(assert-true "it addresses the lattice point as a Threefry counter"
             (string-contains? nsrc
               "threefry4x32(vec4u(bitcast<u32>(ix), bitcast<u32>(iy), bitcast<u32>(iz), 0u)"))
;; The quintic fade, not smoothstep: its first AND second derivatives
;; vanish at both ends, which is what stops the lattice showing as creases.
(assert-true "the fade curve is quintic"
             (string-contains? nsrc "t * t * t * (t * (t * 6.0 - 15.0) + 10.0)"))

;;--- every DECLARED function must actually exist --------------------------
;; The hole in the two-tier design, found the hard way. wgsl-declare!
;; ASSERTS a signature for hand-written WGSL, and nothing checked that the
;; WGSL was there. perlin3v was declared while the edit that included
;; noise.wgsl in the assembled source had silently failed to apply, so the
;; kernel language type-checked the call happily and the shader compiled to
;;
;;   unresolved call target 'perlin3v'
;;
;; in the browser, which is the one place nothing here can see. A signature
;; is a promise; this is the test that the promise is kept.

(define asm (wrangle-wgsl ""))
(for-each
 (lambda (sig)
   (assert-true (string-append "declared " (symbol->string (car sig))
                               " has a definition in the assembled shader")
                (string-contains? asm (string-append "fn " (cadr sig) "("))))
 wgsl-signatures)

;;--- a declared name may not shadow a built-in ---------------------------
;; The rebuilt environment puts declarations first, so a parameter called
;; `seed` would quietly resolve to a parameter slot instead of the
;; uniform's seed — a wrong answer that still renders.
(assert-equal "a parameter may not take a built-in's name"
              'raised (guard (e (#t 'raised)) (wrangle-params! '((seed :u32)))))
(assert-equal "nor may an attribute"
              'raised (guard (e (#t 'raised)) (scratch-attributes! '((time :f32)))))
(assert-equal "ordinary names are still fine"
              'ok (guard (e (#t 'raised)) (begin (wrangle-params! '(sigma)) 'ok)))

(shared-layout! '())
(scratch-attributes! '())        ; leave the world as we found it
(wrangle-params! '(sigma radius gain))

;;--- a wrangle written in Scheme ----------------------------------------
;; wrangle-wgsl stays exactly as it is: raw WGSL remains the escape hatch,
;; and it has earned that — fold-i exists because a real program wanted
;; something this language did not have, and a form that foreclosed the
;; escape would have made that a blocker rather than a request.
;;
;; What Scheme adds is a body that TYPE-CHECKS at expand time: mixing a
;; vec2 with a vec3 becomes an error here, with the form in hand, rather
;; than a shader compile log naming a line of generated text.

(wrangle-params! '(scale gain (warm :flag)))
(scratch-attributes! '((pose :quat) (charge :f32)))

;; The terminal form is the only place statements enter the language, and
;; its type is one no operator accepts — which is what confines it to the
;; last position of a body rather than a rule anyone has to remember.
(assert-equal "a body must end in (point ...)"
              'raised (guard (e (#t 'raised)) (wrangle-scheme '(+ 1.0 2.0))))
(assert-equal "and (point ...) is not a value"
              'raised (guard (e (#t 'raised))
                        (wgsl-type '(+ 1.0 (point position pscale colour)) wrangle-env)))

;; FIELDS ARE TOTAL, because pt_write is one packed write of seven floats
;; and a partial point would have to read back what it did not mention.
;; Naming the input is how you say "unchanged".
(assert-equal "position, size and colour are all required"
              'raised (guard (e (#t 'raised)) (wrangle-scheme '(point position pscale))))
(assert-equal "and they are type-checked"
              'raised (guard (e (#t 'raised))
                        (wrangle-scheme '(point position colour colour))))

;; ATTRIBUTES ARE PARTIAL, because their setters are independent. The
;; asymmetry is legible from the storage layout rather than memorised.
(define psrc (wrangle-scheme '(point position pscale colour)))
(assert-true "a point with no attributes writes only the point"
             (string-contains? psrc "pt_write(i, pt_pos(i), pt_size(i), pt_colour(i));"))
;; Checks for a CALL, not a name: the preamble DEFINES every declared
;; attribute's setter, so the bare name is always present. An earlier form
;; of this assertion matched the definition and failed.
(assert-false "and emits no setter call at all"
              (string-contains? psrc "attr_charge_set(i, "))
(define asrc (wrangle-scheme '(point position pscale colour (charge 0.5))))
(assert-true "naming one emits exactly that setter"
             (string-contains? asrc "attr_charge_set(i, 0.5);"))
(assert-false "and still not the other"
              (string-contains? asrc "attr_pose_set(i, "))

;; Checked against the declaration, so a misspelling fails here with the
;; name in hand rather than at shader compile as an unresolved call.
(assert-equal "an undeclared attribute is refused"
              'raised (guard (e (#t 'raised))
                        (wrangle-scheme '(point position pscale colour (nonesuch 1.0)))))
(assert-equal "as is one given the wrong type"
              'raised (guard (e (#t 'raised))
                        (wrangle-scheme '(point position pscale colour (pose 1.0)))))

;;--- arithmetic applies only to what it applies to ----------------------
;; The rule used to be "reject bool, and if either side is f32 take the
;; other type", which let anything through when paired with a float — a
;; terminal form included — and typed (u32 * 3.0) as u32 while emitting
;; WGSL that will not compile. Naming what IS allowed excludes a new type
;; until someone decides otherwise.
(define ue (cons (cons 'k (cons :u32 "k")) wrangle-env))
(assert-equal "a scalar broadcasts across a vector"
              "(2.0 * pt_colour(i))" (wgsl-code '(* 2.0 colour) ue))
(assert-equal "u32 combines with u32"
              "(k * 3u)" (wgsl-code '(* k (u32 3)) ue))
;; Every bare number emits as f32, so this would have produced "k * 3.0".
(assert-equal "but not with a bare number, which is an f32"
              'raised (guard (e (#t 'raised)) (wgsl-type '(* k 3) ue)))
(assert-equal "nor with a bool"
              'raised (guard (e (#t 'raised)) (wgsl-type '(+ 1.0 (< 1.0 2.0)) ue)))

(wrangle-params! '(sigma radius gain))
(scratch-attributes! '())

(suite-summary)
