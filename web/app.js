// app.js: vxs WebAssembly Creative Concurrency Workbench

(function() {
  'use strict';

  // DOM Elements
  const canvas = document.getElementById('vxs-canvas');
  const ctx = canvas.getContext('2d', { alpha: false });
  // The WebGPU surface is a SEPARATE element; see the comment in
  // index.html. Never call getContext on it here — gpu-run-kernel! does
  // that, and doing it twice with different types is what breaks.
  //
  // No preset draws on the 2D context any more, but the canvas-* shims
  // below stay: they back real Scheme primitives that still work from the
  // REPL. Deleting the demos is not the same as deleting the capability.
  const gpuCanvas = document.getElementById('vxs-gpu-canvas');
  const chkWatch = document.getElementById('chk-watch');
  const watchPath = document.getElementById('watch-path');
  const editor = document.getElementById('code-editor');
  const btnRun = document.getElementById('btn-run');
  const btnRunTests = document.getElementById('btn-run-tests');
  const btnToggleFibers = document.getElementById('btn-toggle-fibers');
  const btnClearCanvas = document.getElementById('btn-clear-canvas');
  const btnClearTerm = document.getElementById('btn-clear-term');
  const selectPreset = document.getElementById('select-preset');
  const statusBadge = document.getElementById('status-badge');
  const statusText = document.getElementById('status-text');
  const terminalBody = document.getElementById('terminal-body');
  const tagFps = document.getElementById('tag-fps');
  const tagFibers = document.getElementById('tag-fibers');
  const tagAlloc = document.getElementById('tag-alloc');
  const tagHeap = document.getElementById('tag-heap');
  const evalTime = document.getElementById('eval-time');
  const mouseCoords = document.getElementById('mouse-coords');
  const fibersToggleText = document.getElementById('fibers-toggle-text');
  const fibersToggleIcon = document.getElementById('fibers-toggle-icon');

  // State
  let vxsEval = null;
  let vxsEvalJson = null;
  let vxsStepFibers = null;
  let vxsLibNames = null;
  let vxsRegisterLib = null;
  let vxsActiveFibersCount = null;
  let vxsClearFibers = null;
  let vxsStatsJson = null;

  // Previous stats sample, so the overlay can report allocation RATE.
  // Cumulative totals are the only way to distinguish "allocates nothing"
  // from "allocates furiously and collects all of it" — a live-bytes
  // reading looks identical in both cases.
  let lastStats = null;
  let isWasmReady = false;
  let fibersRunning = true;
  window.vxsPaused = false;
  let mouseX = 400;
  let mouseY = 300;
  let isMouseDown = false;
  let wheelTotal = 0;

  // FPS Telemetry
  let lastFrameTime = performance.now();
  let frameCount = 0;
  let currentFps = 60;

  // Setup Canvas Dimensions (DPI scaling)
  function resizeCanvas() {
    const rect = canvas.parentElement.getBoundingClientRect();
    if (rect.width > 0 && rect.height > 0) {
      canvas.width = rect.width;
      canvas.height = rect.height;
      // Background fill on resize
      ctx.fillStyle = '#05070a';
      ctx.fillRect(0, 0, canvas.width, canvas.height);
    }
  }
  window.addEventListener('resize', resizeCanvas);
  resizeCanvas();

  // Mouse / touch tracking, on BOTH surfaces. Only one canvas is visible at
  // a time and a hidden element receives no pointer events, so listening
  // only on the 2D canvas meant mouse-x/mouse-y froze for every GPU preset
  // — which is exactly where dragging to orbit a camera is wanted.
  function trackMouse(el) {
    el.addEventListener('mousemove', (e) => {
      const rect = el.getBoundingClientRect();
      mouseX = e.clientX - rect.left;
      mouseY = e.clientY - rect.top;
      mouseCoords.textContent = `Mouse: (${Math.round(mouseX)}, ${Math.round(mouseY)})`;
    });
    el.addEventListener('mousedown', () => { isMouseDown = true; });
    // passive:false so preventDefault actually applies — otherwise the
    // page scrolls away underneath while you are zooming the scene.
    el.addEventListener('wheel', (e) => {
      e.preventDefault();
      wheelTotal += e.deltaY;
    }, { passive: false });
  }
  trackMouse(canvas);
  trackMouse(gpuCanvas);
  window.addEventListener('mouseup', () => { isMouseDown = false; });

  // Expose Drawing Hooks to Wasm (via EM_JS in wasm_api.cpp)
  window.vxsCanvasClear = function(r, g, b, a) {
    ctx.fillStyle = `rgba(${Math.round(r * 255)}, ${Math.round(g * 255)}, ${Math.round(b * 255)}, ${a})`;
    ctx.fillRect(0, 0, canvas.width, canvas.height);
  };

  window.vxsCanvasFillRect = function(x, y, w, h, r, g, b, a) {
    ctx.fillStyle = `rgba(${Math.round(r * 255)}, ${Math.round(g * 255)}, ${Math.round(b * 255)}, ${a})`;
    ctx.fillRect(x, y, w, h);
  };

  window.vxsCanvasDrawCircle = function(x, y, radius, r, g, b, a) {
    ctx.beginPath();
    ctx.arc(x, y, radius, 0, Math.PI * 2);
    ctx.fillStyle = `rgba(${Math.round(r * 255)}, ${Math.round(g * 255)}, ${Math.round(b * 255)}, ${a})`;
    ctx.fill();
  };

  window.vxsCanvasDrawLine = function(x1, y1, x2, y2, r, g, b, a) {
    ctx.beginPath();
    ctx.moveTo(x1, y1);
    ctx.lineTo(x2, y2);
    ctx.strokeStyle = `rgba(${Math.round(r * 255)}, ${Math.round(g * 255)}, ${Math.round(b * 255)}, ${a})`;
    ctx.lineWidth = 1.5;
    ctx.stroke();
  };

  window.vxsCanvasDrawText = function(text, x, y, r, g, b, a) {
    ctx.font = '13px "JetBrains Mono", monospace';
    ctx.fillStyle = `rgba(${Math.round(r * 255)}, ${Math.round(g * 255)}, ${Math.round(b * 255)}, ${a})`;
    ctx.fillText(text, x, y);
  };

  window.vxsCanvasWidth = function() { return canvas.width; };
  window.vxsCanvasHeight = function() { return canvas.height; };
  window.vxsMouseX = function() { return mouseX; };
  window.vxsMouseY = function() { return mouseY; };
  window.vxsMouseDown = function() { return isMouseDown ? 1 : 0; };
  window.vxsMouseWheel = function() { return wheelTotal; };
  // One call per COMPLETE LINE, buffered by the sink port in the VM — so
  // one div per line is now correct. It used to be one div per `display`
  // call, which split `(display "x = ") (display 42)` across two lines.
  // An empty string means a bare (newline), i.e. a blank line.
  window.vxsPrint = function(text) { logToTerm(text, 'val'); };

  // Pages can route a named sink anywhere: (open-output-sink "name") in
  // Scheme writes here. The two built-in names, "terminal" and "console",
  // are handled by the VM's default fallback and need no entry.
  window.vxsSinks = window.vxsSinks || {};

  // Terminal Logging
  function logToTerm(text, type = 'out') {
    const line = document.createElement('div');
    line.className = `terminal-line term-${type}`;
    line.textContent = text;
    terminalBody.appendChild(line);
    terminalBody.scrollTop = terminalBody.scrollHeight;
  }

  // Scheme Execution
  function executeSchemeCode() {
    if (!isWasmReady) {
      logToTerm('Error: WebAssembly module not yet initialized', 'err');
      return;
    }
    const code = editor.value.trim();
    if (!code) return;

    // Stop whatever is already running FIRST. Without this, Run is not
    // idempotent: each press left the previous animation fiber alive and
    // started another, so two presses meant two loops rewriting their own
    // point buffers and each issuing a full-canvas draw. They share one
    // ~8ms wall-clock backstop per frame inside the VM, so the second loop
    // does not run alongside the first so much as starve it — which
    // presents as stutter that looks like it scales with the point count,
    // when what it really scales with is how many times Run was pressed.
    if (vxsClearFibers) vxsClearFibers();

    logToTerm(`=> ${code}`, 'in');
    const t0 = performance.now();
    try {
      const result = vxsEval(code);
      const elapsed = (performance.now() - t0).toFixed(2);
      evalTime.textContent = `Evaluated in ${elapsed} ms`;
      if (result && result !== '#<unspecified>') {
        logToTerm(result, 'out');
      }
    } catch (e) {
      logToTerm(`Runtime Exception: ${e.message}`, 'err');
    }
  }

  // Heap/GC overlay. Rates are per *rendered frame*, differenced against
  // the previous sample rather than read as an absolute.
  function updateHeapTelemetry(time) {
    const s = JSON.parse(vxsStatsJson());
    if (!s.ready) return;

    if (lastStats) {
      const dFrames = Math.max(1, s.step_calls - lastStats.step_calls);
      const dObj = s.total_objects_allocated - lastStats.total_objects_allocated;
      const dBytes = s.total_bytes_allocated - lastStats.total_bytes_allocated;
      const dSec = Math.max(1e-3, (time - lastStats.t) / 1000);
      const perFrame = Math.round(dObj / dFrames);
      const mbPerSec = (dBytes / dSec / (1024 * 1024)).toFixed(1);
      // GC as a RATE, not the cumulative count: a monotonic counter only
      // tells you how long the page has been open. Collections/sec is what
      // says whether pressure is changing.
      const gcPerSec = ((s.gc_count - lastStats.gc_count) / dSec).toFixed(1);
      tagAlloc.textContent = `${perFrame.toLocaleString()} obj/f · ${mbPerSec} MB/s · ${gcPerSec} GC/s`;
      // No color-coding on obj/frame. Absolute churn is workload-relative
      // — 2,000/frame is alarming for 8 points and unremarkable for 512 —
      // so any fixed threshold here is just a permanently-lit warning
      // light, which is worse than none. Judge this against a baseline for
      // the same scene, not against a constant.
      tagAlloc.style.color = 'var(--text-secondary)';

      // THE FPS NUMBER IS NOT THE ANIMATION RATE, and conflating them cost
      // an afternoon: a demo visibly running at about ten frames a second
      // sat under a chip reading 60 FPS. Both were true. requestAnimationFrame
      // fires at 60Hz whether or not the program got anywhere, and the
      // scheduler gives all fibers a shared ~8ms budget per call — so a
      // fiber whose pass takes 27ms is preempted repeatedly and completes
      // one update every fourth browser frame.
      //
      // The honest measure is how often a fiber reaches (yield), which is
      // once per completed pass. An earlier attempt derived it as frames
      // minus preemptions; that is wrong, and a headless run showed it
      // immediately — a badly overrunning fiber is preempted on EVERY step
      // call, so the estimate collapsed to zero while the fiber was in fact
      // completing three passes.
      const dYield = s.total_yields - lastStats.total_yields;
      const scenePerSec = Math.round(dYield / dSec);
      if (dYield > 0 && scenePerSec < currentFps - 5) {
        tagFps.textContent = `${scenePerSec}/s scene · ${currentFps} FPS`;
        tagFps.style.color = 'var(--accent-amber, #e0a030)';
        tagFps.title =
          'The browser is painting at ' + currentFps + ' FPS, but fibers are ' +
          'overrunning the frame budget so the scene only advances ' +
          scenePerSec + ' times a second.';
      } else {
        tagFps.textContent = `${currentFps} FPS`;
        tagFps.style.color = '';
        tagFps.title = '';
      }
    }
    tagHeap.textContent =
      `${s.live_objects.toLocaleString()} live · ${(s.live_bytes / 1024).toFixed(0)} KB`;

    s.t = time;
    lastStats = s;
  }

  // Animation & Fiber Stepping Loop (60 FPS)
  function renderLoop(time) {
    // Compute FPS
    frameCount++;
    if (time - lastFrameTime >= 500) {
      currentFps = Math.round((frameCount * 1000) / (time - lastFrameTime));
      // Text is set by updateHeapTelemetry just below, which also knows
      // whether the scene is keeping up with the paint rate.
      tagFps.textContent = `${currentFps} FPS`;
      // Sample heap counters on the same half-second cadence as FPS —
      // often enough to be live, rare enough that JSON.parse never shows
      // up in the frame budget it is supposed to be measuring.
      if (isWasmReady && vxsStatsJson) updateHeapTelemetry(time);
      frameCount = 0;
      lastFrameTime = time;
    }

    // The pump ALWAYS runs. Pausing is a flag the program reads (see
    // paused? in Scheme), not something done to the scheduler — because
    // the renderer and the camera are fibers too, and stopping them is the
    // opposite of what "pause" should mean here.
    if (isWasmReady) {
      try {
        // 0 = default scheduling: every fiber runs to its own (yield)
        // under a shared ~8ms wall-clock backstop inside the VM. The
        // old positive-number form (an instruction cap per fiber) is
        // debug-only — it preempts fibers at boundaries they didn't
        // choose, which is exactly what the VM now refuses to disguise
        // as normal yielding.
        const hasMore = vxsStepFibers(0);
        const activeCount = vxsActiveFibersCount();
        tagFibers.textContent = `${activeCount} Active Fibers`;
        if (activeCount > 0) {
          tagFibers.style.color = 'var(--accent-green)';
        } else {
          tagFibers.style.color = 'var(--text-secondary)';
        }
      } catch (e) {
        console.error('Error during fiber stepping:', e);
      }
    }

    requestAnimationFrame(renderLoop);
  }

  // Showcase Demo Presets
  const PRESETS = {
    plasma: `;;; ==========================================================
;;; GPU Plasma Field — WGSL compiled from Scheme, in the browser
;;; ==========================================================
;;; Nothing here writes WGSL. The kernel below is compiled to a shader by
;;; lib/wgsl.scm, which type-checks it first: mixing a vec2 with a vec3,
;;; or swizzling past the end of a vector, is a Scheme error here rather
;;; than a shader compilation log in the browser console.
;;;
;;; There is no quote on the kernel body. define-kernel is a macro, so it
;;; receives the form unevaluated — the language boundary is the syntax
;;; rather than a punctuation mark. Inside it you are writing kernel code,
;;; where uv, time and res are the inputs and the result is a vec3 colour.
;;;
;;; lib/*.scm is embedded in the wasm binary, so load works here even
;;; though the browser has no filesystem.

(load "lib/gpu.scm")

(define-kernel plasma
  (let ((c (- uv 0.5))
        (r (length c))
        (a (* 6.2831 (+ (* r 3.0) (* time 0.15)))))
    (vec3 (+ 0.5 (* 0.5 (sin (- (* r 24.0) (* time 3.0)))))
          (+ 0.5 (* 0.5 (cos (+ a (* time 0.7)))))
          (+ 0.6 (* 0.4 (sin (* time 1.3)))))))

;;; The device arrives as an ordinary future; the loop runs one frame per
;;; yield, pumped by the same driver the CPU demos use. All of that lives
;;; in run-kernel-loop, including the rule that guard must wrap only the
;;; draw and never the yield — guard cannot suspend a fiber.
(run-kernel-loop plasma "vxs-gpu-canvas")
`,

    rings: `;;; ==========================================================
;;; GPU Polar Rings — conditionals in the kernel language
;;; ==========================================================
;;; (if c a b) compiles to WGSL select(b, a, c), which is BRANCHLESS: both
;;; arms are evaluated and neither short-circuits. That is the right
;;; default on a GPU, where a real branch diverges the warp — but it means
;;; an arm can never guard the other from a bad value.
;;;
;;; Hard edges are what conditionals buy. Without them every kernel this
;;; language can express is a smooth gradient.

(load "lib/gpu.scm")

(define-kernel rings
  (let ((c (- uv 0.5))
        (r (length c))
        (band (fract (- (* r 12.0) (* time 0.8))))
        (edge (if (< band 0.5) 1.0 0.0))
        (glow (+ 0.35 (* 0.35 (sin (+ (* r 18.0) time))))))
    (vec3 (* edge glow)
          (* edge (+ 0.25 (* 0.5 (fract (+ r (* time 0.2))))))
          (+ 0.35 (* 0.45 (- 1.0 edge))))))

(run-kernel-loop rings "vxs-gpu-canvas")
`,

    ensemble: `;;; ==========================================================
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

(shared-layout! (list (list 'centres (* 3 NACTORS))
                      (list 'radii NACTORS)
                      (list 'tints (* 3 NACTORS))
                      ;; A rotation VECTOR per actor: axis times rate. The
                      ;; kernel turns it into an angle by multiplying by
                      ;; time, so a cloud tumbles on its own axis forever
                      ;; without the host sending anything per frame.
                      (list 'spins (* 3 NACTORS))
                      ;; How many of this actor's bodies are visible. The
                      ;; rest are written at size zero and collapse — the
                      ;; same degenerate trick a released pool slot uses.
                      (list 'shown NACTORS)))
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

;;; --- the expansion ---------------------------------------------------
(define (u32-text n) (string-append (number->string n) "u"))

(define kernel (wrangle-wgsl (string-append "
  let a = i / " (u32-text PER-ACTOR) ";
  let c = vec3<f32>(shared_centres(a * 3u),
                    shared_centres(a * 3u + 1u),
                    shared_centres(a * 3u + 2u));
  let r = shared_radii(a) * w.p0;
  let tint = vec3<f32>(shared_tints(a * 3u),
                       shared_tints(a * 3u + 1u),
                       shared_tints(a * 3u + 2u));

  // Stable within the formation: the preamble seeds from this cube's
  // index and the seed does not change, so a swarm keeps its shape while
  // its owner moves it.
  let dir = normalize(vec3<f32>(random_normal(0.0, 1.0),
                                random_normal(0.0, 1.0),
                                random_normal(0.0, 1.0)));
  let rad = pow(random_uniform(0.0, 1.0), 0.3333333);

  // THE CLOUD TURNS. Each cube's direction within the formation is fixed —
  // drawn once from its own index and never redrawn — so a swarm is a
  // RIGID BODY, and rotating it means rotating that direction. The spin is
  // a rotation vector in the shared buffer, axis times rate, and the angle
  // is that times the clock: a cloud tumbles forever with the host sending
  // nothing per frame.
  let spin = vec3<f32>(shared_spins(a * 3u),
                       shared_spins(a * 3u + 1u),
                       shared_spins(a * 3u + 2u));
  let sd = q_rot(q_from_rotvec(spin * w.time), dir);

  attr_pose_set(i, q_from_rotvec(sd * w.p2));

  // Shape from the actor's RADIUS, which is the one number already in the
  // shared buffer that tracks its role — a scout sits at the floor, an
  // anchor swells above it. Reading it here rather than sending a fourth
  // number keeps the per-actor channel at seven floats.
  //
  // 0 tetrahedron, 1 octahedron, 2 cube: sharp, middling, settled. Because
  // the radius EASES between roles, a swarm passes through the octahedron
  // on the way, so a commitment reads as a shape changing rather than a
  // size popping.
  let big = shared_radii(a);
  attr_shape_set(i, select(select(0u, 1u, big > 0.050), 2u, big > 0.105));

  // HOW MUCH OF THIS NUCLEUS EXISTS. An actor owns a fixed block of
  // bodies but shows only as many as its mass has earned; the rest are
  // written at size zero and collapse to a point — a vertex shader
  // invocation and no fragments. So capture and decay change the SIZE OF
  // THE CLOUD rather than only its colour.
  let local = i - a * " (u32-text PER-ACTOR) ";
  let alive = local < u32(shared_shown(a));
  let sz = select(0.0, 0.006 * w.p1, alive);
  // BRIGHTEN OUTWARD, not inward. The previous form darkened by rad, so
  // the cubes at the surface of a swarm — the only ones that reach the
  // eye — were at 45% while the fully occluded core sat at 100%. The
  // shading was being spent entirely on cubes nobody can see.
  pt_write(i, c + sd * (rad * r), sz,
           tint * (0.70 + 0.35 * rad));
")))

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
`,

    field: `;;; ==========================================================
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
`,

    cubes: `;;; ==========================================================
;;; map cube — the same actors, drawn as solid geometry
;;; ==========================================================
;;; Drag to orbit, scroll to zoom.
;;;
;;; Every dot is a FIBER with its own state and its own control flow. It is
;;; born, it wanders, it spends energy deciding where to go, and when the
;;; energy runs out it dies and hands its slot back. Nothing steps a list
;;; of particles; each actor runs its own loop and yields once per frame.
;;;
;;; Colour is age. A new actor is white-hot; as its energy drains it cools
;;; through amber and red to a dim violet, and then it is gone. Thinking
;;; costs more energy than coasting, so the restless ones cool fastest.
;;;
;;; That distinction is the whole point. A fiber per bouncing ball proves
;;; nothing — an array does that better. A fiber earns its keep when the
;;; thing it models has private evolving state AND its own control flow,
;;; which an agent with intent has and a ball has not.
;;;
;;; It is also the thing a fragment shader cannot do. A shader is a pure
;;; function of position with no memory, no per-entity control flow, and no
;;; way to spawn or retire anything. State would have to be encoded into
;;; textures and ping-ponged, and birth and death are where that model
;;; gives up entirely. Here the CPU decides and the GPU draws.
;;;
;;; This is the actors demo with ONE line changed at the bottom:
;;; run-cubes-loop instead of run-points-loop. Same seven floats per point,
;;; same pool, same fibers — draw(36, N) instead of draw(6, N).
;;;
;;; What differs is all in the shader. A sprite offsets its corners AFTER
;;; projection so it always faces you; a cube offsets them in world space
;;; BEFORE, so it turns as you orbit. That needs a real w for depth and for
;;; perspective-correct interpolation, and depth testing instead of
;;; additive blending — sprites accumulate and order does not matter, solid
;;; geometry occludes and order is everything.

(load "lib/gpu.scm")
(load "lib/actors.scm")
(load "lib/threefry.scm")

(define CAPACITY 500)
(define pool (make-point-pool CAPACITY))
(define cam (make-camera))
(define key (vector 20260821 0 0 0))

;;; Randomness indexed by (actor id, decision number) rather than drawn
;;; from a stream — so an actor's choices depend only on who it is and how
;;; many decisions it has made, never on how many other actors exist or on
;;; the order the scheduler happened to run them in. With hundreds of
;;; fibers interleaving, a shared stream would make every run different and
;;; none of them reproducible.
(define (decide id n)
  (threefry4x32-unit (vector id n 0 0) key))

;;; One actor. Private state lives in the loop variables — position,
;;; heading, energy, and how many decisions it has taken — and is reachable
;;; from nowhere else in the system.
(define (spawn-actor! id)
  (let ((slot (pool-claim! pool)))
    (if slot
        (future
          ;; Two draws at birth, not one: decision 0 sets the heading,
          ;; decision 1 places the actor. Decision indices 1..23 are
          ;; otherwise unused — the first re-think is at 24 — so this costs
          ;; nothing an actor was going to spend anyway.
          (let ((r0 (decide id 0))
                (r1 (decide id 1)))
            ;; Newborns emerge from a REGION rather than a point. Spawning
            ;; every actor at exactly the origin piled the freshest, largest,
            ;; whitest ones on top of each other, and the result read as one
            ;; solid mass — the only part of the picture that looked
            ;; authored rather than grown. The offset is small against the
            ;; swarm's ~0.9 radius, and flattened in y to match the heading
            ;; distribution, so the cloud keeps its shape.
            ;; r1's fourth component was going spare — the position
            ;; offset needs three — so temperament costs no extra draw.
            (let* ((period (+ 6 (inexact->exact
                                 (floor (* 48.0 (vector-ref r1 3)))))))
            (let loop ((x (* 0.20 (- (vector-ref r1 0) 0.5)))
                       (y (* 0.08 (- (vector-ref r1 1) 0.5)))
                       (z (* 0.20 (- (vector-ref r1 2) 0.5)))
                       (hx (- (vector-ref r0 0) 0.5))
                       (hy (* 0.4 (- (vector-ref r0 1) 0.5)))
                       (hz (- (vector-ref r0 2) 0.5))
                       (energy 1.0)
                       (n 1))
              (if (<= energy 0.0)
                  ;; Death: give the slot back. The release blanks the point,
                  ;; so the actor leaves no corpse on screen.
                  (pool-release! pool slot)
                  (let* ((think? (= 0 (modulo n period)))
                         (r (if think? (decide id n) #f))
                         ;; Deciding costs energy, and each actor decides at
                         ;; its OWN rate — drawn at birth, from every six
                         ;; frames to every fifty-four. That is what makes
                         ;; the cost mean anything: with a rate shared by
                         ;; everyone it would be a constant tax, identical
                         ;; for all, and no actor would differ from another.
                         ;;
                         ;; It pushes twice in the same direction. An
                         ;; impulsive actor turns often, so its random walk
                         ;; goes nowhere, AND it pays to turn — near and
                         ;; cold. A committed one holds a heading, travels,
                         ;; and pays less — far and still warm.
                         (nhx (if think? (+ (* 0.6 hx) (- (vector-ref r 0) 0.5)) hx))
                         (nhy (if think? (+ (* 0.6 hy) (* 0.4 (- (vector-ref r 1) 0.5))) hy))
                         (nhz (if think? (+ (* 0.6 hz) (- (vector-ref r 2) 0.5)) hz))
                         (cost (if think? 0.014 0.0035))
                         (nx (+ x (* 0.012 nhx)))
                         (ny (+ y (* 0.012 nhy)))
                         (nz (+ z (* 0.012 nhz)))
                         ;; A soft wall: heading away from the origin costs
                         ;; more, so the swarm stays roughly bounded without
                         ;; anything being clamped.
                         (rad (sqrt (+ (* nx nx) (* ny ny) (* nz nz))))
                         (drag (if (> rad 0.9) 0.010 0.0))
                         (e (- energy cost drag)))
                    ;; Heat death: an actor is white-hot when it is new and
                    ;; cools through amber, red and magenta to a dim violet
                    ;; as its energy goes. The same ramp the GPU wrangle
                    ;; uses, evaluated here because the actor picks its own
                    ;; colour — and read a channel at a time, since a fresh
                    ;; colour vector per actor per frame would have been the
                    ;; largest allocation source in the program.
                    (pool-write-heat! pool slot nx ny nz
                                      (+ 0.006 (* 0.020 e)) e)
                    ;; actor-yield rather than yield: it also holds the
                    ;; actor still while the page is paused, without taking
                    ;; it out of the scheduler — so the renderer keeps
                    ;; drawing and the camera keeps orbiting.
                    (actor-yield)
                    (loop nx ny nz nhx nhy nhz e (+ n 1)))))))))))

;;; A fiber whose whole job is making more fibers. Population is dynamic:
;;; actors die on their own schedule and the spawner refills, so the count
;;; settles wherever birth and death balance rather than being chosen.
(define next-id 0)
(define BIRTHS-PER-FRAME 1)
(future
  (let loop ()
    (let born ((k 0))
      (if (and (not (paused?))
               (< k BIRTHS-PER-FRAME) (< (pool-live pool) CAPACITY))
          (begin (spawn-actor! next-id)
                 (set! next-id (+ next-id 1))
                 (born (+ k 1)))))
    (yield)
    (loop)))

;;; The renderer is just another fiber. It does not know what actors are —
;;; it draws whatever is in the buffer.
(run-cubes-loop (pool-bytes pool) CAPACITY
                (lambda (t) (orbit-camera! cam))
                cam "vxs-gpu-canvas")
`,

    actors: `;;; ==========================================================
;;; Actors — one fiber per agent, writing into the GPU buffer
;;; ==========================================================
;;; Drag to orbit, scroll to zoom.
;;;
;;; Every dot is a FIBER with its own state and its own control flow. It is
;;; born, it wanders, it spends energy deciding where to go, and when the
;;; energy runs out it dies and hands its slot back. Nothing steps a list
;;; of particles; each actor runs its own loop and yields once per frame.
;;;
;;; Colour is age. A new actor is white-hot; as its energy drains it cools
;;; through amber and red to a dim violet, and then it is gone. Thinking
;;; costs more energy than coasting, so the restless ones cool fastest.
;;;
;;; That distinction is the whole point. A fiber per bouncing ball proves
;;; nothing — an array does that better. A fiber earns its keep when the
;;; thing it models has private evolving state AND its own control flow,
;;; which an agent with intent has and a ball has not.
;;;
;;; It is also the thing a fragment shader cannot do. A shader is a pure
;;; function of position with no memory, no per-entity control flow, and no
;;; way to spawn or retire anything. State would have to be encoded into
;;; textures and ping-ponged, and birth and death are where that model
;;; gives up entirely. Here the CPU decides and the GPU draws.

(load "lib/gpu.scm")
(load "lib/actors.scm")
(load "lib/threefry.scm")

(define CAPACITY 900)
(define pool (make-point-pool CAPACITY))
(define cam (make-camera))
(define key (vector 20260821 0 0 0))

;;; Randomness indexed by (actor id, decision number) rather than drawn
;;; from a stream — so an actor's choices depend only on who it is and how
;;; many decisions it has made, never on how many other actors exist or on
;;; the order the scheduler happened to run them in. With hundreds of
;;; fibers interleaving, a shared stream would make every run different and
;;; none of them reproducible.
(define (decide id n)
  (threefry4x32-unit (vector id n 0 0) key))

;;; One actor. Private state lives in the loop variables — position,
;;; heading, energy, and how many decisions it has taken — and is reachable
;;; from nowhere else in the system.
(define (spawn-actor! id)
  (let ((slot (pool-claim! pool)))
    (if slot
        (future
          ;; Two draws at birth, not one: decision 0 sets the heading,
          ;; decision 1 places the actor. Decision indices 1..23 are
          ;; otherwise unused — the first re-think is at 24 — so this costs
          ;; nothing an actor was going to spend anyway.
          (let ((r0 (decide id 0))
                (r1 (decide id 1)))
            ;; Newborns emerge from a REGION rather than a point. Spawning
            ;; every actor at exactly the origin piled the freshest, largest,
            ;; whitest ones on top of each other, and the result read as one
            ;; solid mass — the only part of the picture that looked
            ;; authored rather than grown. The offset is small against the
            ;; swarm's ~0.9 radius, and flattened in y to match the heading
            ;; distribution, so the cloud keeps its shape.
            ;; r1's fourth component was going spare — the position
            ;; offset needs three — so temperament costs no extra draw.
            (let* ((period (+ 6 (inexact->exact
                                 (floor (* 48.0 (vector-ref r1 3)))))))
            (let loop ((x (* 0.20 (- (vector-ref r1 0) 0.5)))
                       (y (* 0.08 (- (vector-ref r1 1) 0.5)))
                       (z (* 0.20 (- (vector-ref r1 2) 0.5)))
                       (hx (- (vector-ref r0 0) 0.5))
                       (hy (* 0.4 (- (vector-ref r0 1) 0.5)))
                       (hz (- (vector-ref r0 2) 0.5))
                       (energy 1.0)
                       (n 1))
              (if (<= energy 0.0)
                  ;; Death: give the slot back. The release blanks the point,
                  ;; so the actor leaves no corpse on screen.
                  (pool-release! pool slot)
                  (let* ((think? (= 0 (modulo n period)))
                         (r (if think? (decide id n) #f))
                         ;; Deciding costs energy, and each actor decides at
                         ;; its OWN rate — drawn at birth, from every six
                         ;; frames to every fifty-four. That is what makes
                         ;; the cost mean anything: with a rate shared by
                         ;; everyone it would be a constant tax, identical
                         ;; for all, and no actor would differ from another.
                         ;;
                         ;; It pushes twice in the same direction. An
                         ;; impulsive actor turns often, so its random walk
                         ;; goes nowhere, AND it pays to turn — near and
                         ;; cold. A committed one holds a heading, travels,
                         ;; and pays less — far and still warm.
                         (nhx (if think? (+ (* 0.6 hx) (- (vector-ref r 0) 0.5)) hx))
                         (nhy (if think? (+ (* 0.6 hy) (* 0.4 (- (vector-ref r 1) 0.5))) hy))
                         (nhz (if think? (+ (* 0.6 hz) (- (vector-ref r 2) 0.5)) hz))
                         (cost (if think? 0.014 0.0035))
                         (nx (+ x (* 0.012 nhx)))
                         (ny (+ y (* 0.012 nhy)))
                         (nz (+ z (* 0.012 nhz)))
                         ;; A soft wall: heading away from the origin costs
                         ;; more, so the swarm stays roughly bounded without
                         ;; anything being clamped.
                         (rad (sqrt (+ (* nx nx) (* ny ny) (* nz nz))))
                         (drag (if (> rad 0.9) 0.010 0.0))
                         (e (- energy cost drag)))
                    ;; Heat death: an actor is white-hot when it is new and
                    ;; cools through amber, red and magenta to a dim violet
                    ;; as its energy goes. The same ramp the GPU wrangle
                    ;; uses, evaluated here because the actor picks its own
                    ;; colour — and read a channel at a time, since a fresh
                    ;; colour vector per actor per frame would have been the
                    ;; largest allocation source in the program.
                    (pool-write-heat! pool slot nx ny nz
                                      (+ 0.003 (* 0.011 e)) e)
                    ;; actor-yield rather than yield: it also holds the
                    ;; actor still while the page is paused, without taking
                    ;; it out of the scheduler — so the renderer keeps
                    ;; drawing and the camera keeps orbiting.
                    (actor-yield)
                    (loop nx ny nz nhx nhy nhz e (+ n 1)))))))))))

;;; A fiber whose whole job is making more fibers. Population is dynamic:
;;; actors die on their own schedule and the spawner refills, so the count
;;; settles wherever birth and death balance rather than being chosen.
(define next-id 0)
(define BIRTHS-PER-FRAME 2)
(future
  (let loop ()
    (let born ((k 0))
      (if (and (not (paused?))
               (< k BIRTHS-PER-FRAME) (< (pool-live pool) CAPACITY))
          (begin (spawn-actor! next-id)
                 (set! next-id (+ next-id 1))
                 (born (+ k 1)))))
    (yield)
    (loop)))

;;; The renderer is just another fiber. It does not know what actors are —
;;; it draws whatever is in the buffer.
(run-points-loop (pool-bytes pool) CAPACITY
                 (lambda (t) (orbit-camera! cam))
                 cam "vxs-gpu-canvas")
`,

    wrangle: `;;; ==========================================================
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
`,

    points: `;;; ==========================================================
;;; GPU Instanced Points — a cube of points in 3D. Drag to orbit.
;;; ==========================================================
;;; Scheme fills a buffer; the GPU draws one quad per point. Seven floats
;;; each — x, y, z, size, r, g, b — flat rather than an array of structs,
;;; because WGSL gives vec3<f32> a 16-byte alignment inside a storage
;;; array and a struct would not pack the way these fields pack here.
;;;
;;; The camera is PARAMETERS, not a matrix: yaw, pitch, distance and fov
;;; ride in the uniform and the vertex shader rotates and projects. There
;;; is no depth buffer either — additive blending is commutative, so a
;;; glowing point cloud is order-independent and never needs sorting.
;;;
;;; Scheme rewrites every point each frame. Watch the obj/f and GC
;;; counters: that CPU cost is what a compute wrangle will delete.

(load "lib/gpu.scm")
(load "lib/threefry.scm")

(define SIDE 14)                       ; SIDE^3 points
(define N (* SIDE SIDE SIDE))
(define buf (make-points N))
(define pts (points-view buf))
(define cam (make-camera))
(define key (vector 0 0 0 0))

(define (grid-coord i) (- (* 2.0 (/ i (- SIDE 1.0))) 1.0))

;;; Base positions and hues are computed ONCE. They do not change between
;;; frames, and working them out per frame — three integer divisions and
;;; three grid-coord calls per point — cost about 40% of the pass for
;;; nothing. What remains in update! is only what actually varies with
;;; time.
;;;
;;; One Threefry block per point gives four independent randoms: jitter on
;;; each axis plus a hue. Indexed by point number, so the cloud is
;;; identical every run with no state and no ordering.
(define bx  (make-vector N 0.0))
(define by  (make-vector N 0.0))
(define bz  (make-vector N 0.0))
(define hue (make-vector N 0.0))

(let fill ((i 0))
  (if (< i N)
      (let ((r  (threefry4x32-unit (vector i 0 0 0) key))
            (ix (modulo i SIDE))
            (iy (modulo (quotient i SIDE) SIDE))
            (iz (quotient i (* SIDE SIDE))))
        (vector-set! bx i (+ (grid-coord ix) (* 0.05 (- (vector-ref r 0) 0.5))))
        (vector-set! by i (+ (grid-coord iy) (* 0.05 (- (vector-ref r 1) 0.5))))
        (vector-set! bz i (+ (grid-coord iz) (* 0.05 (- (vector-ref r 2) 0.5))))
        (vector-set! hue i (vector-ref r 3))
        (fill (+ i 1)))))

(define (update! t)
  (orbit-camera! cam)
  (let loop ((i 0))
    (if (< i N)
        (let* ((x0 (vector-ref bx i))
               (y0 (vector-ref by i))
               (z0 (vector-ref bz i))
               (h  (vector-ref hue i))
               ;; A standing wave through the lattice — a placeholder for
               ;; the noise field a compute wrangle will evaluate.
               (w  (sin (+ (* 2.2 x0) (* 1.7 y0) (* 2.9 z0) t)))
               (d  (* 0.16 w))
               (lit (* 0.5 (+ 1.0 w))))
          (point-set! pts i
                      (+ x0 d) (+ y0 d) (+ z0 d)
                      (+ 0.008 (* 0.010 h))
                      (+ 0.30 (* 0.60 lit))
                      (+ 0.35 (* 0.45 h))
                      (+ 0.55 (* 0.40 (- 1.0 lit))))
          (loop (+ i 1))))))

(run-points-loop buf N update! cam "vxs-gpu-canvas")
`,

    fibers: `;;; ==========================================================
;;; Multi-Fiber Task Concurrency with Future & Touch
;;; ==========================================================

(define (task name steps base-val)
  (future
    (let loop ((i 1) (acc base-val))
      (yield)
      (if (< i steps)
          (loop (+ i 1) (+ acc i))
          acc))))

;; Spawn 3 concurrent futures with different step counts
(define f1 (task 'alpha 4 100))
(define f2 (task 'beta 6 200))
(define f3 (task 'gamma 8 300))

(display "Spawned 3 concurrent tasks in background fibers.\n")
(display "Alpha (4 steps) = ") (display (touch f1)) (newline)
(display "Beta  (6 steps) = ") (display (touch f2)) (newline)
(display "Gamma (8 steps) = ") (display (touch f3)) (newline)
(list (touch f1) (touch f2) (touch f3))
`,

    repl: `;;; ==========================================================
;;; High-Order Scheme Functional Programming
;;; ==========================================================

;; Fast recursive factorial
(define (fact n)
  (if (<= n 1)
      1
      (* n (fact (- n 1)))))

(display "Factorial 10 = ")
(display (fact 10))
(newline)

;; Higher-order closures & lexical capture
(define (make-adder x)
  (lambda (y) (+ x y)))

(define add10 (make-adder 10))
(display "add10(32) = ")
(display (add10 32))
(newline)

;; Macro expansion
(when (= (+ 2 2) 4)
  (display "When macro works beautifully!")
  (newline))
`
  };

  // Which presets draw through WebGPU rather than the 2D context.
  const GPU_PRESETS = { plasma: true, rings: true, points: true, wrangle: true, actors: true, cubes: true, field: true, ensemble: true };

  function showSurface(preset) {
    const wantsGpu = !!GPU_PRESETS[preset];
    canvas.style.display = wantsGpu ? 'none' : '';
    gpuCanvas.style.display = wantsGpu ? '' : 'none';
  }

  //--- watch mode --------------------------------------------------------
  // Edit .scm files in a real editor and have the page re-run them on save.
  //
  // The browser has no filesystem, but it has fetch, and the static server
  // is already serving the repo. So watch mode polls the demo file AND
  // every lib/*.scm over HTTP, and on any change re-registers the
  // libraries and re-evaluates the demo.
  //
  // Re-registering the libraries is the part that matters: lib/*.scm is
  // compiled into the wasm binary at build time, so a library edit is
  // normally invisible here until a rebuild — which bit us once already,
  // when `if` in the kernel language worked natively and did not exist in
  // the page. vxs_register_lib installs an override that `load` prefers.
  //
  // Requires serving from the REPO ROOT, not web/, so that /lib and /demos
  // are reachable:  python3 -m http.server  then open /web/index.html
  const WATCH_INTERVAL_MS = 1000;
  let watchTimer = null;
  let watchSeen = {};

  async function fetchText(url) {
    const res = await fetch(url, { cache: 'no-store' });
    if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
    return res.text();
  }

  async function watchPass(force) {
    const demoUrl = watchPath.value.trim();
    if (!demoUrl) return;

    const names = (vxsLibNames ? vxsLibNames() : '').split(',').filter(Boolean);
    const fresh = {};
    let changed = !!force;

    for (const name of names) {
      const url = '../lib/' + name;
      try {
        const text = await fetchText(url);
        fresh[url] = text;
        if (watchSeen[url] !== text) changed = true;
      } catch (e) {
        // Not fatal — the baked-in copy still works — but not silent
        // either. A 404 here usually means the server is rooted at web/
        // instead of the repo, and swallowing it turns that into "watch
        // mode mysteriously ignores my library edits".
        if (force) {
          logToTerm(`watch: cannot fetch ${url} — ${e.message} ` +
                    `(serving from the repo root?)`, 'err');
        }
      }
    }

    let demo;
    try {
      demo = await fetchText(demoUrl);
    } catch (e) {
      if (force) logToTerm(`watch: cannot fetch ${demoUrl} — ${e.message}`, 'err');
      return;
    }
    if (watchSeen[demoUrl] !== demo) changed = true;
    if (!changed) return;

    for (const name of names) {
      const url = '../lib/' + name;
      if (fresh[url] !== undefined) vxsRegisterLib(name, fresh[url]);
    }
    watchSeen = fresh;
    watchSeen[demoUrl] = demo;

    editor.value = demo;
    // Watch demos drive the GPU surface; that is what they are for.
    canvas.style.display = 'none';
    gpuCanvas.style.display = '';
    logToTerm(`\n--- watch: ${demoUrl} changed, re-running ---`, 'meta');
    executeSchemeCode();
  }

  function setWatching(on) {
    if (watchTimer) { clearInterval(watchTimer); watchTimer = null; }
    if (!on) { logToTerm('watch: off', 'meta'); return; }
    watchSeen = {};
    logToTerm(`watch: polling ${watchPath.value.trim()} and lib/*.scm every ${WATCH_INTERVAL_MS}ms`, 'meta');
    watchPass(true);
    watchTimer = setInterval(() => watchPass(false), WATCH_INTERVAL_MS);
  }

  chkWatch.addEventListener('change', () => setWatching(chkWatch.checked));
  watchPath.addEventListener('change', () => { if (chkWatch.checked) setWatching(true); });

  // One path for loading a preset, used by both the dropdown and startup.
  // They used to be separate, and startup hardcoded PRESETS.particles while
  // the dropdown showed whatever option happened to be first — so the page
  // booted running one demo while claiming to be running another. Sharing
  // the function is what makes that disagreement unrepresentable.
  function loadPreset(name) {
    if (!PRESETS[name]) return;
    if (vxsClearFibers) vxsClearFibers();
    showSurface(name);
    ctx.fillStyle = '#05070a';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    editor.value = PRESETS[name];
    executeSchemeCode();
  }

  // Preset Selection Event
  selectPreset.addEventListener('change', () => {
    const val = selectPreset.value;
    if (!PRESETS[val]) {
      // Was a silent no-op, which is the worst possible answer: selecting
      // a preset appeared to do nothing at all, with nothing in the log to
      // explain it. In practice this means index.html reloaded while
      // app.js came from cache, so the dropdown offers an option this
      // script has never heard of.
      logToTerm(`No preset named "${val}" in this script — app.js is stale, ` +
                `almost certainly cached. Serve with "python3 serve.py" from ` +
                `the repo root, which disables caching. To force it now: ` +
                `Cmd-Option-R in Safari, Cmd-Shift-R elsewhere.`, 'err');
      return;
    }
    logToTerm(`\n--- Loaded preset: [${selectPreset.options[selectPreset.selectedIndex].text}] ---`, 'meta');
    loadPreset(val);
  });

  // Same drift, caught at startup rather than on the click: every option in
  // the markup should name a preset this script defines.
  Array.from(selectPreset.options).forEach((opt) => {
    if (!PRESETS[opt.value]) {
      console.warn(`vxs: <option value="${opt.value}"> has no matching preset in app.js`);
    }
  });

  // Buttons
  btnRun.addEventListener('click', executeSchemeCode);

  editor.addEventListener('keydown', (e) => {
    if ((e.metaKey || e.ctrlKey) && e.key === 'Enter') {
      e.preventDefault();
      executeSchemeCode();
    }
    // Tab key support
    if (e.key === 'Tab') {
      e.preventDefault();
      const start = editor.selectionStart;
      const end = editor.selectionEnd;
      editor.value = editor.value.substring(0, start) + '  ' + editor.value.substring(end);
      editor.selectionStart = editor.selectionEnd = start + 2;
    }
  });

  btnToggleFibers.addEventListener('click', () => {
    fibersRunning = !fibersRunning;
    window.vxsPaused = !fibersRunning;
    if (fibersRunning) {
      fibersToggleIcon.textContent = '⏸';
      fibersToggleText.textContent = 'Running';
      btnToggleFibers.style.borderColor = 'var(--accent-green)';
    } else {
      fibersToggleIcon.textContent = '▶';
      fibersToggleText.textContent = 'Paused — drag to orbit';
      btnToggleFibers.style.borderColor = 'var(--accent-amber)';
    }
  });

  btnRunTests.addEventListener('click', async () => {
    if (!isWasmReady || !vxsEvalJson) {
      logToTerm('Wasm engine is not ready yet.', 'err');
      return;
    }

    logToTerm('\n══════════════════════════════════════════════════', 'meta');
    logToTerm('🧪 RUNNING IN-BROWSER SCHEME UNIT TEST MATRIX...', 'meta');
    logToTerm('══════════════════════════════════════════════════', 'meta');

    if (typeof runTestSuite === 'function') {
      const summary = await runTestSuite(vxsEvalJson, vxsClearFibers);
      for (const r of summary.results) {
        if (r.passed) {
          logToTerm(`  ✓ ${r.name} (${r.elapsedMs} ms) -> ${r.resObj.result || '[error handled]'}`, 'val');
        } else {
          logToTerm(`  ✗ ${r.name} (${r.elapsedMs} ms): ${r.failureReason}`, 'err');
        }
      }
      logToTerm('──────────────────────────────────────────────────', 'meta');
      if (summary.failed === 0) {
        logToTerm(`✨ ALL ${summary.total} UNIT TESTS PASSED IN BROWSER! ✨`, 'val');
      } else {
        logToTerm(`💥 ${summary.failed} of ${summary.total} tests failed.`, 'err');
      }
    }
  });

  btnClearCanvas.addEventListener('click', () => {
    if (vxsClearFibers) vxsClearFibers();
    ctx.fillStyle = '#05070a';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    logToTerm('Canvas & active fibers cleared.', 'meta');
  });

  btnClearTerm.addEventListener('click', () => {
    terminalBody.innerHTML = '';
  });

  // Wasm Initializer (createVxsModule Promise)
  // CACHE-BUST THE WASM. vxs.wasm is rebuilt on every make, and a browser
  // that reuses a cached copy produces the most confusing failure this
  // project can generate: a fix that provably landed appearing not to have
  // — the old shader compiling, the old error printing, with the file on
  // disk already correct.
  //
  // serve.py sends no-store, but nothing makes anyone use serve.py, and
  // `python3 -m http.server` does not. So the page asks for a URL the
  // cache has never seen instead of asking politely not to be cached.
  //
  // locateFile is emscripten's hook for where the .wasm lives, which is
  // the fetch that actually matters — vxs.js being fresh is no help if the
  // binary beside it is stale.
  createVxsModule({
    locateFile: (path) => (path.endsWith('.wasm') ? path + '?t=' + Date.now() : path),
  }).then((Module) => {
    try {
      Module._vxs_init();
      vxsEval = Module.cwrap('vxs_eval', 'string', ['string']);
      vxsEvalJson = Module.cwrap('vxs_eval_json', 'string', ['string']);
      vxsStepFibers = Module.cwrap('vxs_step_fibers', 'number', ['number']);
      vxsActiveFibersCount = Module.cwrap('vxs_active_fibers_count', 'number', []);
      vxsClearFibers = Module.cwrap('vxs_clear_fibers', null, []);
      vxsStatsJson = Module.cwrap('vxs_stats_json', 'string', []);
      vxsLibNames = Module.cwrap('vxs_lib_names', 'string', []);
      vxsRegisterLib = Module.cwrap('vxs_register_lib', null, ['string', 'string']);

      isWasmReady = true;
      statusBadge.classList.add('active');
      statusText.textContent = 'Wasm Engine Ready';
      logToTerm('✓ WebAssembly NaN-Boxed Scheme Core initialized successfully (250 KB).', 'meta');
      logToTerm('✓ C++20 Fiber Coroutine Scheduler hooked into requestAnimationFrame (60 FPS).', 'meta');

      // WHICH BINARY IS THIS. vxs.wasm is a committed artifact and browsers
      // cache it eagerly, so a fix can be present on disk and absent in the
      // tab — indistinguishable, from inside the tab, from a fix that did
      // not work. Compare this against `make -s buildstamp` and the
      // question stops being arguable.
      try {
        const stamp = Module.ccall('vxs_eval', 'string', ['string'], ['(vxs-build)']);
        logToTerm('✓ engine build ' + String(stamp).replace(/^"|"$/g, '').trim(), 'meta');
      } catch (e) { /* an older binary has no stamp, which is itself the answer */ }

      // Load whatever the dropdown is actually showing.
      loadPreset(selectPreset.value);
    } catch (e) {
      statusText.textContent = 'Init Error';
      logToTerm(`Initialization error: ${e.message}`, 'err');
    }
  }).catch((err) => {
    statusText.textContent = 'Wasm Load Failed';
    logToTerm(`Failed to load WebAssembly binary: ${err.message}`, 'err');
  });

  // Start Animation Loop
  requestAnimationFrame(renderLoop);

})();
