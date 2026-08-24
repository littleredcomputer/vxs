;;; ==========================================================
;;; Ensemble — stage 2: roles, sensing, and a field that moves
;;; ==========================================================
;;; Drag to orbit, scroll to zoom.
;;;
;;; Ninety-six fibers decide; sixty-one thousand cubes are
;;; decided upon. Stage 1 proved the expansion. This stage
;;; gives the actors something to decide ABOUT.
;;;
;;; There is an invisible field drifting through the volume.
;;; Nothing draws it — what you see is entirely the ensemble's
;;; RESPONSE to it, which is the point: the picture is the
;;; behaviour, not the data.
;;;
;;; TWO ROLES, AND THEY ARE DIFFERENT PROGRAMS:
;;;
;;;   SCOUT   small, dim, quick. Wanders. Samples where it
;;;           lands, and on finding something, commits.
;;;   ANCHOR  large, bright, still. Sits on what it found and
;;;           swells with its claim. When the field drifts out
;;;           from under it, it lets go and scouts again.
;;;
;;; That divergence is the thing a shader cannot do cheaply. A
;;; warp running thirty-two different plans runs all thirty-two;
;;; a fiber runs only its own. Here half the ensemble is
;;; executing code the other half is not.
;;;
;;; SENSING IS EXPENSIVE, SO ACTORS DO NOT DO IT OFTEN. One
;;; evaluation of the field costs ~70us in Scheme, and doing
;;; ninety-six of them every frame would eat two thirds of the
;;; budget. So each actor senses on its own schedule, staggered
;;; by index: twelve evaluations a frame, perfectly flat.
;;;
;;; An actor can choose not to think. A kernel invocation has
;;; no such option — it recomputes everything, every frame,
;;; whether or not anything changed.
;;;
;;; TWO KNOBS, both live from the REPL:
;;;   (set! PACE 0.15)   slower, to watch a hand-off happen
;;;   (set! DRIFT x)     0.0 to 1.0, described below
;;;   1.0  the stage-1 picture — prescribed orbits, no sensing
;;;   0.0  pure seeking
;;; Anything between dissolves one into the other.
;;; ==========================================================

(load "lib/noise.scm")

(define NACTORS 96)
(define PER-ACTOR 640)
(define N (* NACTORS PER-ACTOR))
(define SENSE-EVERY 8)         ; frames between an actor's samples

(define DRIFT 0.0)             ; 1.0 = stage 1, 0.0 = pure seeking
(define FIELD-SCALE 1.9)
;;; Fast enough that a feature crosses a lattice cell in a couple of
;;; seconds. Slower and anchors are never dislodged, so the ensemble
;;; ratchets to all-anchors and stops being about anything.
(define FIELD-SPEED 0.55)

;;; THE OTHER KNOB. Everything that moves is scaled by this — the field's
;;; drift, the actors' approach, the simulation clock — so slowing down to
;;; watch a hand-off does not also change the equilibrium. Tuning the three
;;; constants separately would.
(define PACE 0.40)
(define FIELD-SEED 20260823)
;;; MEASURED, not guessed. Sampling the field over the volume:
;;;   |v| > 0.30 covers 12% of space,  |v| > 0.18 covers 32%.
;;; So a wandering scout finds something about one sample in eight, and
;;; the GAP between the two is hysteresis — an anchor commits at 0.30 and
;;; does not let go until 0.18, so it holds through the field's wobble
;;; instead of flickering on the threshold. Commitment is the thing an
;;; actor has and a pure function of the present does not.
(define CLAIM-ON  0.30)
(define CROWD 0.30)            ; two anchors closer than this contend
(define CLAIM-OFF 0.18)

;;; HEAVIER IS LESS STABLE — literally true of nuclei, and mechanically
;;; necessary here. Without it a nucleus grows without bound: scouts
;;; respawn after capture and are eaten again, so one body reached a mass
;;; of 100 out of 96 actors, ate the ensemble, and the whole thing then
;;; collapsed to ninety-six bare scouts and an empty screen.
;;;
;;; Raising the release threshold with mass makes growth self-limiting.
;;; The practical ceiling is where the threshold meets CLAIM-ON, since no
;;; ground is strong enough to hold a nucleus past that.
(define INSTABILITY 0.0045)
(define (release-threshold a)
  (+ CLAIM-OFF (* INSTABILITY (- (vector-ref mass a) 1.0))))

;;; Quasiquote, now that these live in a file rather than inside a
;;; JavaScript template literal where a backtick would have closed the
;;; string. Sizes are COMPUTED, which is exactly what a literal with holes
;;; in it is for — and the alternative was (list (list ...)) six deep.
(shared-layout! `((centres ,(* 3 NACTORS))
                  (radii   ,NACTORS)
                  (tints   ,(* 3 NACTORS))
                  ;; A rotation VECTOR per actor: axis times rate. The
                  ;; kernel turns it into an angle by multiplying by time,
                  ;; so a cloud tumbles on its own axis forever without the
                  ;; host sending anything per frame.
                  (spins   ,(* 3 NACTORS))
                  ;; How many of this actor's bodies are visible. The rest
                  ;; are written at size zero and collapse — the same
                  ;; degenerate trick a released pool slot uses.
                  (shown   ,NACTORS)))
(define W (make-shared))
(define WV (shared-view W))

;;; Two stock attributes. 'pose' turns each body; 'shape' picks which
;;; solid it is. Shape reads role faster than colour does, and unlike a
;;; tint it survives being small and dim.
(scratch-attributes! '((pose :quat) (shape :u32)))
(define SCRATCH (make-scratch N))

(wrangle-params! '(spread size twist))
(define P (make-wrangle-params))
(define PV (wrangle-params-view P))
(param-set! PV 'spread 1.0)
(param-set! PV 'size   0.85)
(param-set! PV 'twist  1.1)

(define bodies (make-points N))

;;; --- per-actor state, as parallel vectors ---------------------------
(define px (make-vector NACTORS 0.0))
(define py (make-vector NACTORS 0.0))
(define pz (make-vector NACTORS 0.0))
(define tx (make-vector NACTORS 0.0))
(define ty (make-vector NACTORS 0.0))
(define tz (make-vector NACTORS 0.0))
(define claim (make-vector NACTORS 0.0))

;;; MASS, in absorbed scouts. A nucleus grows by capture and scatters when
;;; it decays. It is the one quantity in the demo that is neither sensed
;;; nor decided but ACCUMULATED — a history, which is the third thing a
;;; fiber has that a kernel invocation cannot.
(define mass (make-vector NACTORS 1.0))
(define SCOUT-BODIES 70)
;; Chosen so the display saturates near the mass the instability actually
;; permits (~20), rather than at 7 — otherwise every nucleus past a modest
;; size looks identical and the growth stops being visible.
(define BODIES-PER-MASS 30)

;;; A SHORT MEMORY OF HAVING LOST. Without it an actor that yields walks a
;;; little way off, senses the same feature it was just beaten on,
;;; re-anchors, and is beaten again — the pair trading places twice a
;;; second, which is what the event log kept showing. Refusing to commit
;;; for a few sensing periods turns that thrash into territory.
;;;
;;; This is memory, which is the other thing a fiber has and a kernel
;;; invocation does not: a kernel begins every dispatch knowing nothing.
(define cooldown (make-vector NACTORS 0))
(define COOLDOWN-PERIODS 5)

;;; WHAT IS DRAWN, as against what is decided. A role change is instant —
;;; an actor commits in one frame — but making the PICTURE change in one
;;; frame snaps a swarm between 0.035 and 0.21, a sixfold jump, thirty-four
;;; times a second across the ensemble. That is the chop.
;;;
;;; So the decision stays instant and the appearance eases toward it. The
;;; actor knows immediately; the swarm takes about a third of a second to
;;; agree. Nothing about the dynamics changes — only what you can follow.
(define dr (make-vector NACTORS 0.035))
(define dR (make-vector NACTORS 0.22))
(define dG (make-vector NACTORS 0.62))
(define dB (make-vector NACTORS 1.00))
(define EASE 0.10)

(define (field x y z t)
  (perlin3 (* x FIELD-SCALE)
           (* y FIELD-SCALE)
           (- (* z FIELD-SCALE) (* t FIELD-SPEED))
           FIELD-SEED))

;;; Reporting is EVENTS ONLY, and rate-limited. Ninety-six actors
;;; narrating every frame is not talkative, it is noise.
(define chatter 0)
(define chatter-window 0)
(define (report! text)
  (let ((w (quotient (frame-count) 60)))
    (if (not (= w chatter-window))
        (begin (set! chatter-window w) (set! chatter 0)))
    (if (< chatter 3)
        (begin (set! chatter (+ chatter 1)) (display text) (newline)))))

(define frames 0)
(define (frame-count) frames)

(define (actor-write! a cx cy cz r tr tg tb)
  (let ((k (* a 3)))
    (shared-set! WV 'shown a
                 (min PER-ACTOR (+ SCOUT-BODIES
                                   (* BODIES-PER-MASS (- (vector-ref mass a) 1.0)))))
    (shared-set! WV 'centres k cx)
    (shared-set! WV 'centres (+ k 1) cy)
    (shared-set! WV 'centres (+ k 2) cz)
    (shared-set! WV 'radii a r)
    (shared-set! WV 'tints k tr)
    (shared-set! WV 'tints (+ k 1) tg)
    (shared-set! WV 'tints (+ k 2) tb)))

;;; The stage-1 orbit, kept so DRIFT can blend back to it.
(define (orbit-x a t)
  (let ((f (/ (exact->inexact a) NACTORS)))
    (* (+ 0.45 (* 0.55 (- 1.0 (* f f)))) (cos (+ (* 2.39996 a) (* t 0.3))))))
(define (orbit-y a t)
  (* 0.5 (sin (+ (* 2.39996 a) (* t 0.21)))))
(define (orbit-z a t)
  (let ((f (/ (exact->inexact a) NACTORS)))
    (* (+ 0.45 (* 0.55 (- 1.0 (* f f)))) (sin (+ (* 2.39996 a) (* t 0.3))))))

(define (mix a b m) (+ (* a (- 1.0 m)) (* b m)))
(define (wander) (- (* 1.7 (random 1000) 0.001) 0.85))

;;; --- the two programs -----------------------------------------------
;;; A scout and an anchor are separate procedures, and the transition
;;; between them is in TAIL POSITION — the last thing either loop does.
;;; Calling one from inside the other's loop body would leave the old
;;; loop's frame on the stack forever, one per role change, and there are
;;; thirty-four of those a second.

(define (yield-away! a rival)
  (let* ((dx (- (vector-ref px a) (vector-ref px rival)))
         (dy (- (vector-ref py a) (vector-ref py rival)))
         (dz (- (vector-ref pz a) (vector-ref pz rival)))
         (d  (max 0.001 (sqrt (+ (* dx dx) (* dy dy) (* dz dz)))))
         (k  (/ (* 2.5 CROWD) d)))
    (vector-set! tx a (max -0.9 (min 0.9 (+ (vector-ref px a) (* dx k)))))
    (vector-set! ty a (max -0.6 (min 0.6 (+ (vector-ref py a) (* dy k)))))
    (vector-set! tz a (max -0.9 (min 0.9 (+ (vector-ref pz a) (* dz k)))))))

(define (fresh-target! a)
  (vector-set! tx a (wander))
  (vector-set! ty a (* 0.6 (wander)))
  (vector-set! tz a (wander)))

(define (sense-at a t)
  (abs (field (vector-ref px a) (vector-ref py a) (vector-ref pz a) t)))

(define (be-scout a t)
  (fresh-target! a)
  (be-scout/keeping-target a t))

;; Same program, but honouring a target someone else already chose — used
;; when an actor is leaving somewhere rather than looking for somewhere.
(define (be-scout/keeping-target a t)
  (vector-set! claim a 0.0)
  (let loop ((t t))
    (step-toward! a (* 0.045 PACE) t)
    (paint-scout a)
    (yield)
    (let ((v (if (sensing? a) (sense-at a t) #f))
          (t2 (+ t (tick))))
      (cond
       ((and v (> (vector-ref cooldown a) 0))
        (vector-set! cooldown a (- (vector-ref cooldown a) 1))
        (if (< (dist2-to-target a) 0.02) (fresh-target! a))
        (loop t2))
       ;; CAPTURED. The scout's mass joins the nucleus and the scout is
       ;; sent far away to begin again — the actor survives, its substance
       ;; does not. Cooling down on the way out stops it being recaptured
       ;; by the same nucleus before it has cleared the reach.
       ((and v (captor-of a))
        => (lambda (b)
             (vector-set! mass b (+ (vector-ref mass b) (vector-ref mass a)))
             (vector-set! mass a 1.0)
             (vector-set! cooldown a COOLDOWN-PERIODS)
             (yield-away! a b)
             (loop t2)))
       ((and v (> v CLAIM-ON))
        (report! (string-append "actor " (number->string a)
                                " scout -> ANCHOR, claim " (number->string v)))
        (vector-set! claim a v)
        (be-anchor a t2))
       (else
        ;; Re-target only on a SENSING frame. Checking every frame means a
        ;; scout that has arrived picks a new direction sixty times a
        ;; second and random-walks in place, which is both erratic to watch
        ;; and covers ground it has already rejected.
        (if (and v (< (dist2-to-target a) 0.02)) (fresh-target! a))
        (loop t2))))))

(define (be-anchor a t)
  (vector-set! tx a (vector-ref px a))
  (vector-set! ty a (vector-ref py a))
  (vector-set! tz a (vector-ref pz a))
  (let loop ((t t))
    (step-toward! a (* 0.010 PACE) t)
    (paint-anchor a)
    (yield)
    (let ((t2 (+ t (tick))))
      (if (not (sensing? a))
          (loop t2)
          (let ((v (sense-at a t)))
            (vector-set! claim a v)
            (let ((rival (out-claimed? a)))
              (cond
               (rival
                (report! (string-append "actor " (number->string a)
                                        " yields to " (number->string rival)))
                (vector-set! claim a 0.0)
                ;; LEAVE, do not merely stand down. A yielding actor that
                ;; stays put re-senses the same ground within a few frames,
                ;; re-anchors, and is beaten again — the thrash visible in
                ;; the log as the same pair trading twice a second. Pushing
                ;; away from the winner turns that into territory.
                ;; Losing the ground costs the mass too: a displaced
                ;; nucleus leaves as a bare scout.
                (vector-set! mass a 1.0)
                (vector-set! cooldown a COOLDOWN-PERIODS)
                (yield-away! a rival)
                (be-scout/keeping-target a t2))
               ((< v (release-threshold a))
                (report! (string-append "actor " (number->string a)
                                        " decays, scattering "
                                        (number->string (- (vector-ref mass a) 1.0))))
                ;; DECAY. What it absorbed is not conserved — it scatters.
                ;; Conserving it would need somewhere to put it, and the
                ;; ninety-six actors are already all alive; a nucleus that
                ;; shatters simply stops being heavy.
                (vector-set! mass a 1.0)
                (be-scout a t2))
               (else (loop t2)))))))))

;;; --- contention -------------------------------------------------------
;;; The one rule that makes this an ENSEMBLE rather than ninety-six agents
;;; each solving their own problem: two anchors cannot hold the same
;;; ground. The weaker claim yields.
;;;
;;; Checked only by the actors that are sensing this frame — twelve of
;;; them — so it costs about a thousand comparisons a frame rather than
;;; the nine thousand a full pairwise sweep would.
;;;
;;; Note what this needs that a kernel cannot have: an actor must know
;;; another actor's claim, decide it is beaten, and CHANGE WHAT PROGRAM IT
;;; IS RUNNING. Not a different value — a different continuation.
;;; Which nucleus, if any, has this scout inside it. Capture radius scales
;;; with mass, so a heavy nucleus reaches further — which is what makes
;;; growth self-reinforcing and gives the ensemble a few dominant bodies
;;; rather than ninety-six equal ones.
(define (captor-of a)
  (let loop ((b 0))
    (cond ((= b NACTORS) #f)
          ((or (= b a) (= 0.0 (vector-ref claim b))) (loop (+ b 1)))
          (else
           (let* ((dx (- (vector-ref px b) (vector-ref px a)))
                  (dy (- (vector-ref py b) (vector-ref py a)))
                  (dz (- (vector-ref pz b) (vector-ref pz a)))
                  (reach (* CROWD (+ 0.55 (* 0.16 (vector-ref mass b))))))
             (if (< (+ (* dx dx) (* dy dy) (* dz dz)) (* reach reach))
                 b
                 (loop (+ b 1))))))))

(define (out-claimed? a)
  (let ((mine (vector-ref claim a)))
    (let loop ((b 0))
      (cond ((= b NACTORS) #f)
            ((or (= b a) (= 0.0 (vector-ref claim b))) (loop (+ b 1)))
            (else
             (let ((dx (- (vector-ref px b) (vector-ref px a)))
                   (dy (- (vector-ref py b) (vector-ref py a)))
                   (dz (- (vector-ref pz b) (vector-ref pz a))))
               (if (and (< (+ (* dx dx) (* dy dy) (* dz dz)) (* CROWD CROWD))
                        (> (vector-ref claim b) mine))
                   b
                   (loop (+ b 1)))))))))

;;; --- shared mechanics -----------------------------------------------
(define (sensing? a) (= 0 (modulo (+ frames a) SENSE-EVERY)))

(define (dist2-to-target a)
  (let ((dx (- (vector-ref tx a) (vector-ref px a)))
        (dy (- (vector-ref ty a) (vector-ref py a)))
        (dz (- (vector-ref tz a) (vector-ref pz a))))
    (+ (* dx dx) (* dy dy) (* dz dz))))

;;; DRIFT blends the sensed target against the stage-1 orbit. At 1.0 the
;;; sensing still happens and simply stops mattering, which is what makes
;;; the knob a dissolve rather than a switch.
(define (step-toward! a rate t)
  (let* ((gx (mix (vector-ref tx a) (orbit-x a t) DRIFT))
         (gy (mix (vector-ref ty a) (orbit-y a t) DRIFT))
         (gz (mix (vector-ref tz a) (orbit-z a t) DRIFT))
         (r (if (> DRIFT 0.5) (* 0.10 PACE) rate)))
    (vector-set! px a (mix (vector-ref px a) gx r))
    (vector-set! py a (mix (vector-ref py a) gy r))
    (vector-set! pz a (mix (vector-ref pz a) gz r))))

(define (ease-toward! a rad tr tg tb)
  (vector-set! dr a (mix (vector-ref dr a) rad EASE))
  (vector-set! dR a (mix (vector-ref dR a) tr EASE))
  (vector-set! dG a (mix (vector-ref dG a) tg EASE))
  (vector-set! dB a (mix (vector-ref dB a) tb EASE))
  (actor-write! a (vector-ref px a) (vector-ref py a) (vector-ref pz a)
                (vector-ref dr a)
                (vector-ref dR a) (vector-ref dG a) (vector-ref dB a)))

;;; Bright enough to be PRESENT. A scout is subordinate, not absent, and
;;; watching one wander into a bright patch is the moment this is about.
;;; Saturated, not tinted grey. Against black, a colour with all three
;;; channels near each other reads as dim no matter how bright it is —
;;; what makes something look lit is the SPREAD between channels.
(define (paint-scout a) (ease-toward! a 0.035 0.22 0.62 1.00))

(define (paint-anchor a)
  (let* ((c (vector-ref claim a))
         ;; Spread the LIVE range across the ramp. Scaling from zero puts
         ;; every anchor at the top of it, since none of them are below
         ;; CLAIM-OFF by definition.
         (hot (max 0.0 (min 1.0 (/ (- c CLAIM-OFF) 0.28)))))
    ;; Violet when barely holding, amber when strong — two ends of a real
    ;; ramp rather than one colour getting lighter. The blue channel FALLS
    ;; as the red rises, which is what makes the two states distinguishable
    ;; at a glance across a crowded volume.
    (ease-toward! a
                  (+ 0.05 (* 0.16 hot))
                  (+ 0.55 (* 0.45 hot))
                  (+ 0.14 (* 0.62 hot))
                  (- 0.85 (* 0.68 hot)))))

;;; --- the expansion, in Scheme --------------------------------------
;;; No WGSL text. lib/wgsl.scm compiles this and type-checks it first, so
;;; mixing a vec2 with a vec3, or handing an attribute the wrong type, is
;;; an error HERE with the form in hand rather than a shader compile log
;;; naming a line of generated text.
;;;
;;; wrangle-wgsl is still there and still the escape hatch — this is the
;;; same kernel it used to hold, not a smaller one.
;;;
;;; PER-ACTOR reaches the shader through quasiquote rather than as a second
;;; copy of the number. Getting that wrong would not fail: bodies would
;;; simply obey the wrong actor and render something entirely plausible.
(define kernel
  (wrangle-scheme
   `(let* (;; Who commands me. Integer division, so each block of
           ;; consecutive bodies belongs to one actor — which also means an
           ;; actor's swarm is contiguous in memory and one coherent read.
           (a     (/ index (u32 ,PER-ACTOR)))
           (a3    (* a (u32 3)))
           (c     (vec3 (shared-centres a3)
                        (shared-centres (+ a3 (u32 1)))
                        (shared-centres (+ a3 (u32 2)))))
           (r     (* (shared-radii a) spread))
           (tint  (vec3 (shared-tints a3)
                        (shared-tints (+ a3 (u32 1)))
                        (shared-tints (+ a3 (u32 2)))))

           ;; A stable station within the formation. The preamble has
           ;; already seeded the generator from this body's index, and the
           ;; seed does not change between frames, so these draws are the
           ;; SAME every frame — a body holds its place while the formation
           ;; moves.
           (dir   (normalize (vec3 (random-normal 0.0 1.0)
                                   (random-normal 0.0 1.0)
                                   (random-normal 0.0 1.0))))
           ;; Cube root of a uniform gives uniform density in the ball;
           ;; without it every swarm is a shell with nothing inside.
           (rad   (pow (random-uniform 0.0 1.0) 0.3333333))

           ;; THE CLOUD TURNS. Because each body's direction is fixed, a
           ;; swarm is a RIGID BODY and rotating it means rotating that
           ;; direction. The spin is a rotation vector — axis times rate —
           ;; and the angle is that times the clock, so a cloud tumbles
           ;; forever with the host sending nothing per frame.
           (spin  (vec3 (shared-spins a3)
                        (shared-spins (+ a3 (u32 1)))
                        (shared-spins (+ a3 (u32 2)))))
           (sd    (q-rot (q-from-rotvec (* spin time)) dir))

           ;; HOW MUCH OF THIS NUCLEUS EXISTS. An actor owns a fixed block
           ;; of bodies but shows only as many as its mass has earned; the
           ;; rest are written at size zero and collapse to a point — a
           ;; vertex shader invocation and no fragments. So capture and
           ;; decay change the SIZE OF THE CLOUD, not only its colour.
           (local (- index (* a (u32 ,PER-ACTOR))))
           (alive (< local (u32 (shared-shown a))))

           ;; Shape from the actor's RADIUS, the one number already in the
           ;; shared buffer that tracks its role. 0 tetrahedron, 1
           ;; octahedron, 2 cube: sharp, middling, settled. Because the
           ;; radius EASES between roles, a swarm passes through the
           ;; octahedron on the way, so a commitment reads as a shape
           ;; changing hands rather than a size popping.
           (big   (shared-radii a)))

      ;; BRIGHTEN OUTWARD, not inward: the bodies at the surface of a swarm
      ;; are the only ones that reach the eye, so that is where the range
      ;; belongs.
      (point (+ c (* sd (* rad r)))
             (if alive (* 0.006 size) 0.0)
             (* tint (+ 0.70 (* 0.35 rad)))
             (pose (q-from-rotvec (* sd twist)))
             (shape (if (> big 0.105) (u32 2)
                        (if (> big 0.050) (u32 1) (u32 0))))))))

(define cam (make-camera))
(camera-distance-set! cam 3.0)
(define (frame! t) (set! frames (+ frames 1)) (orbit-camera! cam))
(define (tick) (* 0.016 PACE))

(display "ensemble: ") (display NACTORS) (display " actors, ")
(display N) (display " bodies. Sensing every ") (display SENSE-EVERY)
(display " frames, staggered. (set! DRIFT 1.0) for the stage-1 picture.")
(newline)

;;; Each actor tumbles on its own axis, assigned once. Golden-angle
;;; phasing again, so neighbours do not turn together and the ensemble
;;; reads as ninety-six independent things rather than one system.
(do ((a 0 (+ a 1))) ((= a NACTORS))
  (vector-set! px a (orbit-x a 0.0))
  (vector-set! py a (orbit-y a 0.0))
  (vector-set! pz a (orbit-z a 0.0))
  (let* ((u (* 2.39996 a))
         (k (* a 3))
         (rate (+ 0.12 (* 0.30 (/ (exact->inexact (modulo (* a 7) NACTORS)) NACTORS)))))
    (shared-set! WV 'spins k       (* rate (cos u)))
    (shared-set! WV 'spins (+ k 1) (* rate (sin (* 0.7 u))))
    (shared-set! WV 'spins (+ k 2) (* rate (sin u))))
  (future (be-scout a 0.0)))

(run-wrangle-loop bodies N kernel frame! cam
                  :canvas "vxs-gpu-canvas"
                  :params P
                  :shared W
                  :scratch SCRATCH
                  :draw :cubes)
