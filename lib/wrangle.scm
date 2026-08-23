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

;; THREE SOURCES, ONE RESULT, REBUILT. The environment is derived from the
;; built-ins plus whatever wrangle-params! and scratch-attributes! were
;; last given — never appended to. Appending looks fine because the newest
;; binding shadows the old one, and leaves every previous name still
;; resolving to a slot nobody writes any more.
(define (wrangle-rebuild-env!)
  (set! wrangle-env
        (append (wrangle-attr-env) (wrangle-param-env) wrangle-builtin-env)))

(define wrangle-env wrangle-builtin-env)

;;--- live parameters ----------------------------------------------------
;; (wrangle-params! '(sigma radius gain)) names the spare uniform slots.
;;
;; ONE declaration is the single source of truth for both sides: the kernel
;; writes `sigma` and it compiles to w.p0, and the host writes 'sigma and
;; it lands at float 0 of the parameter block. A positional (param 0) would
;; work too and would let the two drift apart silently, which is the whole
;; class of bug this project keeps finding.
;; Parameters are TYPED, and the type decides which slot they land in:
;;
;;   (wrangle-params! '(sigma radius gain))                  ; all :f32
;;   (wrangle-params! '((sigma :f32) (mode :u32) (flat :flag)))
;;
;;   :f32   -> p0..p7, one of eight float slots
;;   :u32   -> i0..i2, an honest integer slot
;;   :flag  -> one bit of `flags`
;;
;; ONE declarator rather than three, because three would each do a third of
;; the same job and each need its own setter. A bare symbol still means
;; :f32, so nothing written before this needs changing.
;;
;; :u32 AND :flag ARE NOT TIDINESS. An f32 has 24 mantissa bits, so an
;; integer or a bitfield carried in a float slot works perfectly up to bit
;; 23 and then silently starts dropping — the worst possible failure curve,
;; because it survives every test anyone writes early. The seed had exactly
;; this bug and it cost real time to find.
;;
;; What this removes: recompiling a shader to toggle a mode, keeping two
;; kernels because one of them has a flag set, and `if (mode > 0.5)` — a
;; float comparison standing in for a bit test.

(define wrangle-param-slots 8)    ; p0..p7
(define wrangle-int-slots 3)      ; i0..i2
(define wrangle-flag-bits 32)     ; bits of `flags`

;; ((name type slot) ...) — slot is the index within that type's class.
(define wrangle-params '())

;; Block layout, in 32-bit words: eight floats, then flags, then i0..i2.
;; It does NOT mirror the struct — the host maps one to the other — so the
;; float slots keep the offsets they have always had.
(define wrangle-param-words 12)
(define wrangle-flags-word 8)
(define wrangle-int-word0 9)

(define (wrangle-params! specs)
  (let loop ((ss specs) (nf 0) (ni 0) (nb 0) (acc '()))
    (if (null? ss)
        (begin (set! wrangle-params (reverse acc))
               (wrangle-rebuild-env!)
               wrangle-params)
        (let* ((spec (car ss))
               (name (if (pair? spec) (car spec) spec))
               (type (if (pair? spec)
                         (if (pair? (cdr spec)) (cadr spec)
                             (error "wrangle-params!: expected (name type)" spec))
                         :f32)))
          (if (not (symbol? name))
              (error "wrangle-params!: parameter names must be symbols" spec))
          (cond
           ((eq? type :f32)
            (if (>= nf wrangle-param-slots)
                (error "wrangle-params!: at most 8 :f32 parameters" name))
            (loop (cdr ss) (+ nf 1) ni nb (cons (list name type nf) acc)))
           ((eq? type :u32)
            (if (>= ni wrangle-int-slots)
                (error "wrangle-params!: at most 3 :u32 parameters" name))
            (loop (cdr ss) nf (+ ni 1) nb (cons (list name type ni) acc)))
           ((eq? type :flag)
            (if (>= nb wrangle-flag-bits)
                (error "wrangle-params!: at most 32 :flag parameters" name))
            (loop (cdr ss) nf ni (+ nb 1) (cons (list name type nb) acc)))
           (else (error "wrangle-params!: type must be :f32, :u32 or :flag" spec)))))))

;; A kernel says the name and means the value. A flag reads as :bool, so it
;; drops straight into (if flat a b) with no new machinery.
(define (wrangle-param-env)
  (map (lambda (p)
         (let ((name (car p)) (type (cadr p)) (slot (caddr p)))
           (cond
            ((eq? type :f32)
             (cons name (cons :f32 (string-append "w.p" (number->string slot)))))
            ((eq? type :u32)
             (cons name (cons :u32 (string-append "w.i" (number->string slot)))))
            (else
             (cons name
                   (cons :bool
                         (string-append "((w.flags & (1u << " (number->string slot)
                                        "u)) != 0u)")))))))
       wrangle-params))

;; Wrangle bodies are still WGSL text, so flags also get a helper function
;; — the same dual treatment scratch attributes get.
(define (wrangle-flag-preamble)
  (let ((flags (let loop ((ps wrangle-params) (acc '()))
                 (cond ((null? ps) (reverse acc))
                       ((eq? (cadr (car ps)) :flag) (loop (cdr ps) (cons (car ps) acc)))
                       (else (loop (cdr ps) acc))))))
    (if (null? flags)
        ""
        (apply string-append
               (map (lambda (p)
                      (string-append
                       "fn flag_" (wgsl-fn-name (car p))
                       "() -> bool { return (w.flags & (1u << "
                       (number->string (caddr p)) "u)) != 0u; }\n"))
                    flags)))))

;; The parameter block: eight floats, a flag word and three integer slots,
;; owned by the host and rewritten in place.
;;
;; A BYTES OBJECT, made once and mutated, rather than a fresh list per
;; frame. At sixty frames a second a list would allocate every frame, and
;; the demos this feeds run at zero objects per frame — which is the
;; property that lets them hold a frame budget at all.
(define (make-wrangle-params)
  (let ((b (make-bytes (* 4 wrangle-param-words))))
    (bytes-seal! b)   ; the GPU will read it; it must not move
    b))

;; Two views over the same bytes, so the integer slots are written as
;; integers rather than squeezed through a float.
(define (wrangle-params-view p) (cons (bytes-view p :f32) (bytes-view p :u32)))

(define (param-decl name)
  (let loop ((ps wrangle-params))
    (cond ((null? ps)
           (error "param: undeclared parameter — call wrangle-params! first" name))
          ((eq? (caar ps) name) (car ps))
          (else (loop (cdr ps))))))

(define (param-set! v name value)
  (let* ((d (param-decl name)) (type (cadr d)) (slot (caddr d)))
    (cond ((eq? type :f32) (view-set! (car v) slot value))
          ((eq? type :u32) (view-set! (cdr v) (+ wrangle-int-word0 slot) value))
          (else
           (let ((bit (u32-shl 1 slot))
                 (cur (view-ref (cdr v) wrangle-flags-word)))
             (view-set! (cdr v) wrangle-flags-word
                        (if value (u32-or cur bit) (u32-and cur (u32-not bit)))))))))

(define (param-ref v name)
  (let* ((d (param-decl name)) (type (cadr d)) (slot (caddr d)))
    (cond ((eq? type :f32) (view-ref (car v) slot))
          ((eq? type :u32) (view-ref (cdr v) (+ wrangle-int-word0 slot)))
          (else (not (= 0 (u32-and (view-ref (cdr v) wrangle-flags-word)
                                   (u32-shl 1 slot))))))))

;; Kept as a name because tests and callers use it, but it is no longer an
;; independent number that could disagree — see points-stride-wgsl.
(define wrangle-stride points-stride)

;;--- named scratch attributes -------------------------------------------
;; State a wrangle needs that the renderer must not see: weight, age,
;; velocity, an ancestor index. Until now a kernel could only touch what
;; the renderer already reads, so anything stateful had to be smuggled
;; through a colour channel or not exist at all.
;;
;; A SECOND BUFFER, not a wider point stride. The renderer is the hot path
;; and the kernel is not: widening the stride would make the vertex shader
;; fetch every scratch float for every point of every frame to use none of
;; them. Two further reasons point the same way — a second buffer need not
;; hold one element per point (a histogram, per-workgroup partials, a
;; cumulative array), and reading one column back costs one column rather
;; than the whole 1.7MB point buffer.
;;
;; DECLARED, not dynamic. VEX makes attributes up as it goes; a GPU buffer
;; cannot, because allocation has to precede dispatch. So the declaration
;; is the single source of truth, exactly as wrangle-params! is, and it
;; generates the accessors for both sides rather than asking anyone to
;; agree with anything by hand.
;;
;;   (scratch-attributes! '((weight :f32) (age :f32) (ancestor :u32)))
;;
;; TYPES MATTER, and :u32 is not decoration. An ancestor index is an
;; integer; stored as a float it aliases past 2^24, which is the same bug
;; the seed carried. The buffer is array<f32> and integer attributes are
;; bitcast in and out — free, and exact in both directions. On the host the
;; same trick is two views over the same bytes.
;;
;; :vec3f is three flat floats with the accessor building the vector, as
;; pt_pos already does, because vec3<f32> carries 16-byte alignment inside
;; a storage array and that is a trap better sidestepped than documented.

(define scratch-attrs '())     ; ((name type offset) ...)
(define scratch-stride 0)      ; floats per element

(define (attr-width type)
  (cond ((eq? type :f32) 1)
        ((eq? type :u32) 1)
        ((eq? type :vec3f) 3)
        (else (error "scratch-attributes!: unknown type" type))))

(define (scratch-attributes! specs)
  (let loop ((ss specs) (off 0) (acc '()))
    (if (null? ss)
        (begin
          (set! scratch-attrs (reverse acc))
          (set! scratch-stride off)
          (wrangle-rebuild-env!)
          scratch-stride)
        (let ((spec (car ss)))
          (if (or (not (pair? spec)) (not (pair? (cdr spec))))
              (error "scratch-attributes!: expected (name type)" spec))
          (loop (cdr ss)
                (+ off (attr-width (cadr spec)))
                (cons (list (car spec) (cadr spec) off) acc))))))

(define (scratch-attr name)
  (let loop ((as scratch-attrs))
    (cond ((null? as) (error "scratch: undeclared attribute" name))
          ((eq? (caar as) name) (car as))
          (else (loop (cdr as))))))

(define (scratch-offset name) (caddr (scratch-attr name)))
(define (scratch-type name) (cadr (scratch-attr name)))

;; A kernel says `weight` and means THIS point's weight, the way VEX means
;; @weight. Every wrangle invocation owns exactly one index, so binding the
;; name to the call is both correct and the shorter thing to write.
(define (wrangle-attr-env)
  (map (lambda (a)
         (cons (car a)
               (cons (if (eq? (cadr a) :u32) :f32 (cadr a))
                     (string-append (if (eq? (cadr a) :u32) "f32(attr_" "attr_")
                                    (wgsl-fn-name (car a)) "(i)"
                                    (if (eq? (cadr a) :u32) ")" "")))))
       scratch-attrs))

(define (scratch-stride-wgsl) (string-append (number->string scratch-stride) "u"))

;; The binding and its accessors. Emitted only when something is declared —
;; a shader that declares a storage buffer it never reads is still a
;; different pipeline layout, and every kernel written before today should
;; compile to exactly the text it did before.
(define (scratch-preamble)
  (if (null? scratch-attrs)
      ""
      (apply string-append
             "@group(0) @binding(2) var<storage, read_write> scratch : array<f32>;\n"
             (map (lambda (a) (scratch-accessors (car a) (cadr a) (caddr a)))
                  scratch-attrs))))

(define (scratch-accessors name type off)
  (let ((n (wgsl-fn-name name))
        (base (string-append "i * " (scratch-stride-wgsl)
                             " + " (number->string off) "u")))
    (cond
     ((eq? type :f32)
      (string-append
       "fn attr_" n "(i : u32) -> f32 { return scratch[" base "]; }\n"
       "fn attr_" n "_set(i : u32, v : f32) { scratch[" base "] = v; }\n"))
     ((eq? type :u32)
      ;; bitcast, not a numeric conversion: an ancestor index must survive
      ;; the round trip exactly, and f32 cannot hold every u32.
      (string-append
       "fn attr_" n "(i : u32) -> u32 { return bitcast<u32>(scratch[" base "]); }\n"
       "fn attr_" n "_set(i : u32, v : u32) { scratch[" base "] = bitcast<f32>(v); }\n"))
     ((eq? type :vec3f)
      (string-append
       "fn attr_" n "(i : u32) -> vec3<f32> {\n"
       "  let b = " base ";\n"
       "  return vec3<f32>(scratch[b], scratch[b + 1u], scratch[b + 2u]);\n"
       "}\n"
       "fn attr_" n "_set(i : u32, v : vec3<f32>) {\n"
       "  let b = " base ";\n"
       "  scratch[b] = v.x; scratch[b + 1u] = v.y; scratch[b + 2u] = v.z;\n"
       "}\n"))
     (else (error "scratch-accessors: unknown type" type)))))

;;--- shared read-only data ----------------------------------------------
;; Data every element reads, rather than data each element owns.
;;
;; The scratch buffer cannot hold it: scratch is addressed as
;; scratch[i * stride + off], so it is per-element by construction. The
;; parameter block is eight floats. And anything that changes per frame
;; cannot be baked into the source without recompiling the shader, which is
;; the problem live parameters were added to solve.
;;
;; So: a third storage buffer, bound at 3, READ-ONLY, indexed freely.
;;
;;   (shared-layout! '((walls 48) (obs 41)))
;;
;; Declared as named REGIONS rather than left as a bare array, for the same
;; reason everything else here is declared: an offset computed by hand in
;; two places is an offset that will eventually disagree with itself, and
;; the failure is a plausible wrong picture rather than an error. Each
;; region gets a generated accessor and a registered signature, so a kernel
;; calls (shared-obs k) and never writes an offset at all.
;;
;; The WGSL identifier is `sdata`, not `shared` — `shared` is a reserved
;; word in WGSL and a binding named that will not compile.

(define shared-regions '())    ; ((name offset length) ...)
(define shared-length 0)       ; total floats

(define (shared-layout! specs)
  (let loop ((ss specs) (off 0) (acc '()))
    (if (null? ss)
        (begin
          (set! shared-regions (reverse acc))
          (set! shared-length off)
          (for-each
           (lambda (r)
             (wgsl-declare! (string->symbol (string-append "shared-"
                                                           (symbol->string (car r))))
                            (string-append "shared_" (wgsl-fn-name (car r)))
                            (list :u32) :f32))
           shared-regions)
          shared-length)
        (let ((spec (car ss)))
          (if (or (not (pair? spec)) (not (pair? (cdr spec)))
                  (not (symbol? (car spec)))
                  (not (integer? (cadr spec))) (< (cadr spec) 1))
              (error "shared-layout!: expected (name length), length >= 1" spec))
          (loop (cdr ss) (+ off (cadr spec))
                (cons (list (car spec) off (cadr spec)) acc))))))

(define (shared-region name)
  (let loop ((rs shared-regions))
    (cond ((null? rs) (error "shared: undeclared region" name))
          ((eq? (caar rs) name) (car rs))
          (else (loop (cdr rs))))))

(define (shared-offset name) (cadr (shared-region name)))
(define (shared-size name) (caddr (shared-region name)))

;; Emitted only when something is declared, so a kernel that reads no
;; shared data keeps exactly the bind group layout it had.
(define (shared-preamble)
  (if (null? shared-regions)
      ""
      (apply string-append
             "@group(0) @binding(3) var<storage, read> sdata : array<f32>;\n"
             (map (lambda (r)
                    (string-append
                     "fn shared_" (wgsl-fn-name (car r)) "(k : u32) -> f32 { return sdata["
                     (number->string (cadr r)) "u + k]; }\n"))
                  shared-regions))))

(define (make-shared) 
  (let ((b (make-bytes (* shared-length 4))))
    (bytes-seal! b)
    b))

(define (shared-view b) (bytes-view b :f32))

(define (shared-set! v name k value)
  (let ((r (shared-region name)))
    (if (or (< k 0) (>= k (caddr r)))
        (error "shared-set!: index outside the region" name k))
    (view-set! v (+ (cadr r) k) value)))

(define (shared-ref v name k)
  (let ((r (shared-region name)))
    (if (or (< k 0) (>= k (caddr r)))
        (error "shared-ref: index outside the region" name k))
    (view-ref v (+ (cadr r) k))))

;;--- the host side ------------------------------------------------------
;; Two views over the same bytes give the host the same bitcast the shader
;; does, for nothing.
(define (make-scratch n)
  (let ((b (make-bytes (* n scratch-stride 4))))
    (bytes-seal! b)
    b))

(define (scratch-view b) (cons (bytes-view b :f32) (bytes-view b :u32)))

(define (scratch-set! sv i name . vals)
  (let* ((a (scratch-attr name))
         (k (+ (* i scratch-stride) (caddr a)))
         (type (cadr a)))
    (cond ((eq? type :u32) (view-set! (cdr sv) k (car vals)))
          ((eq? type :vec3f)
           (view-set! (car sv) k (car vals))
           (view-set! (car sv) (+ k 1) (cadr vals))
           (view-set! (car sv) (+ k 2) (caddr vals)))
          (else (view-set! (car sv) k (car vals))))))

(define (scratch-ref sv i name)
  (let* ((a (scratch-attr name))
         (k (+ (* i scratch-stride) (caddr a)))
         (type (cadr a)))
    (cond ((eq? type :u32) (view-ref (cdr sv) k))
          ((eq? type :vec3f)
           (list (view-ref (car sv) k)
                 (view-ref (car sv) (+ k 1))
                 (view-ref (car sv) (+ k 2))))
          (else (view-ref (car sv) k)))))

(define wrangle-preamble
  (string-append
   "\n"
   "struct WU {\n"
   "  time : f32,\n"
   "  count : u32,\n"
   "  seed : u32,\n"
   "  step : u32,\n"
   ;; Integers carried AS integers. An f32 has 24 mantissa bits, so a
   ;; bitfield or a count in a float slot works perfectly to bit 23 and
   ;; then silently drops the rest — which survives every test written
   ;; early. The seed had exactly this bug.
   "  flags : u32,\n"
   "  i0 : u32,\n"
   "  i1 : u32,\n"
   "  i2 : u32,\n"
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
   ))

;; Everything from here down is the entry point. Split from the block above
;; so generated bindings and accessors can be spliced BETWEEN them: WGSL
;; wants a function declared before the code that calls it.
(define wrangle-main-preamble
  (string-append
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
                   wrangle-preamble
                   (wrangle-flag-preamble)
                   (scratch-preamble)
                   (shared-preamble)
                   wrangle-main-preamble body "\n}\n")))
