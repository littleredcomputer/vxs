// app.js: vxs WebAssembly Creative Concurrency Workbench

(function() {
  'use strict';

  // DOM Elements
  const canvas = document.getElementById('vxs-canvas');
  const ctx = canvas.getContext('2d', { alpha: false });
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
  let mouseX = 400;
  let mouseY = 300;
  let isMouseDown = false;

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

  // Mouse / Touch tracking on Canvas
  canvas.addEventListener('mousemove', (e) => {
    const rect = canvas.getBoundingClientRect();
    mouseX = e.clientX - rect.left;
    mouseY = e.clientY - rect.top;
    mouseCoords.textContent = `Mouse: (${Math.round(mouseX)}, ${Math.round(mouseY)})`;
  });

  canvas.addEventListener('mousedown', () => { isMouseDown = true; });
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
  window.vxsPrint = function(text) {
    if (text !== '') logToTerm(text, 'val');
  };

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
    let s;
    try {
      s = JSON.parse(vxsStatsJson());
    } catch (e) {
      return;
    }
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
      tagFps.textContent = `${currentFps} FPS`;
      // Sample heap counters on the same half-second cadence as FPS —
      // often enough to be live, rare enough that JSON.parse never shows
      // up in the frame budget it is supposed to be measuring.
      if (isWasmReady && vxsStatsJson) updateHeapTelemetry(time);
      frameCount = 0;
      lastFrameTime = time;
    }

    if (isWasmReady && fibersRunning) {
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
    particles: `;;; ==========================================================
;;; Concurrent Particle Fibers Demo
;;; ==========================================================
;;; Each particle is driven by its own independent concurrent fiber.
;;; Notice how they all yield cooperatively each frame!

;; Trail fade background
(define (fade)
  (canvas-fill-rect 0 0 (canvas-width) (canvas-height) 0.02 0.03 0.05 0.15))

;; Spawn a single particle fiber
(define (spawn-particle id x y vx vy r g b radius)
  (future
    (let loop ((px x) (py y) (vx vx) (vy vy) (life 0))
      (let* ((w (canvas-width))
             (h (canvas-height))
             (nvx (if (or (< px radius) (> px (- w radius))) (- vx) vx))
             (nvy (if (or (< py radius) (> py (- h radius))) (- vy) vy))
             (nx (+ px nvx))
             (ny (+ py nvy))
             (pulse (+ 0.6 (* 0.4 (sin (* life 0.05))))))
        
        ;; Draw glowing particle
        (canvas-draw-circle nx ny (* radius pulse) (* r pulse) (* g pulse) (* b pulse) 0.9)
        
        ;; Cooperative yield to next browser frame!
        (yield)
        (loop nx ny nvx nvy (+ life 1))))))

;; Main animation controller fiber
(future
  (let loop ()
    (fade)
    (yield)
    (loop)))

;; Spawn 35 concurrent particle fibers with varied velocities and colors
(do ((i 0 (+ i 1)))
    ((= i 35) 'particles-spawned)
  (let ((x (+ 100 (random 600)))
        (y (+ 100 (random 400)))
        (vx (- (random 8) 4))
        (vy (- (random 8) 4))
        (r (random 1.0))
        (g (random 1.0))
        (b (random 1.0))
        (rad (+ 3 (random 6))))
    (spawn-particle i x y (if (= vx 0) 2 vx) (if (= vy 0) 2 vy) r g b rad)))
`,

    wrangle: `;;; ==========================================================
;;; Point-Wrangle: fields, nodes, and the one bridge between them
;;; ==========================================================
;;; Two algebras, and the split is the whole point:
;;;
;;;   NODE   points -> points   touches memory; a future compute dispatch
;;;   FIELD  pos    -> value    pure; a future WGSL *function*
;;;
;;; Fields compose with each other (warp, curl, zoom); exactly one
;;; bridge -- advect -- turns a field into a node. Watch the flow
;;; character change every ~3s: that is \`switch\` routing FIELDS, while
;;; everything downstream of it stays untouched.

(define (v3 x y z) (vector x y z))
(define (v3x p) (vector-ref p 0))
(define (v3y p) (vector-ref p 1))
(define (v3z p) (vector-ref p 2))
(define (v3+ a b) (v3 (+ (v3x a) (v3x b)) (+ (v3y a) (v3y b)) (+ (v3z a) (v3z b))))
(define (v3- a b) (v3 (- (v3x a) (v3x b)) (- (v3y a) (v3y b)) (- (v3z a) (v3z b))))
(define (v3scale s a) (v3 (* s (v3x a)) (* s (v3y a)) (* s (v3z a))))

;;; ---------------------------- FIELDS ----------------------------
;;; A field is an ordinary unary procedure, pos -> value. What it
;;; CLOSES OVER (t, frequency) is a future GPU uniform; its STRUCTURE
;;; is the future shader. That is why rebuilding a field every frame
;;; below costs a closure, never a recompile.

(define (wave t)
  (lambda (p)
    (* 0.5 (+ (sin (+ (v3x p) t))
              (cos (+ (* 1.3 (v3y p)) (* 0.9 t)))
              (sin (+ (* 0.7 (v3z p)) (* 1.1 t)))))))

(define (fscale s f) (lambda (p) (v3scale s (f p))))   ; scales OUTPUT
(define (fzoom  s f) (lambda (p) (f (v3scale s p))))   ; scales DOMAIN
(define (warp f g)   (lambda (p) (f (v3+ p (g p)))))   ; domain distortion

;; One partial derivative, by central difference. curl needs only 6 of
;; the 9 partials, so asking for them individually costs 12 field
;; evaluations instead of the 18 that three full gradients would.
;; (A compiler could differentiate the field tree symbolically and pay
;; ~3 -- this runtime cost is exactly the argument for that.)
(define eps 0.05)
(define (partial f axis)
  (lambda (p)
    (let ((h (v3 (if (= axis 0) eps 0.0)
                 (if (= axis 1) eps 0.0)
                 (if (= axis 2) eps 0.0))))
      (/ (- (f (v3+ p h)) (f (v3- p h))) (* 2.0 eps)))))

;; Three scalar potentials -> a DIVERGENCE-FREE vector field. That is
;; what stops advected points collapsing into sinks: they swirl, they
;; do not clump.
(define (curl3 y1 y2 y3)
  (let ((d3y (partial y3 1)) (d2z (partial y2 2))
        (d1z (partial y1 2)) (d3x (partial y3 0))
        (d2x (partial y2 0)) (d1y (partial y1 1)))
    (lambda (p)
      (v3 (- (d3y p) (d2z p))
          (- (d1z p) (d3x p))
          (- (d2x p) (d1y p))))))

;;; ---------------------------- NODES -----------------------------
;;; @P/@ptnum are deliberately unhygienic ambient bindings the macro
;;; injects -- that is the point of the sigil, and here it is an
;;; ordinary macro rather than VEX's parser hack.

(defmacro (point-wrangle points . body)
  \`(let ((n# (vector-length ,points)))
     (do ((i# 0 (+ i# 1)))
         ((= i# n#))
       (let ((@P (vector-ref ,points i#))
             (@ptnum i#))
         (vector-set! ,points i# (begin ,@body))))))

(define (as-node wrangle-thunk)
  (lambda (points) (wrangle-thunk points) points))

(define (pipe . nodes)
  (lambda (points)
    (for-each (lambda (n) (n points)) nodes)
    points))

;; Routes procedures. It has no idea whether they are nodes or fields,
;; which is why the same combinator works at both levels.
(define (switch selector . branches)
  (lambda (x) ((list-ref branches (selector)) x)))

;; THE bridge: field -> node. The only place the two algebras meet.
(define (advect f dt)
  (as-node (lambda (pts)
             (point-wrangle pts (v3+ @P (v3scale dt (f @P)))))))

;; A node that is not field-driven at all -- keeps the cloud in frame.
;; Note it only acts OUTSIDE radius r. A uniform shrink cannot work
;; here: curl flow is divergence-free, so it neither expands nor
;; contracts volume, and nothing would oppose a constant inward pull --
;; the cloud would collapse to a point. Acting only on escapees gives
;; the ball of radius r as a genuine equilibrium.
(define (contain r k)
  (as-node (lambda (pts)
             (point-wrangle pts
               (let ((d (sqrt (+ (* (v3x @P) (v3x @P))
                                 (* (v3y @P) (v3y @P))
                                 (* (v3z @P) (v3z @P))))))
                 (if (> d r)
                     (v3scale (/ (+ r (* (- 1.0 k) (- d r))) d) @P)
                     @P))))))

(define (make-point-cube n spacing)
  (let* ((total (* n n n))
         (points (make-vector total))
         (offset (* -0.5 spacing (- n 1))))
    (do ((i 0 (+ i 1)))
        ((= i total) points)
      (let ((xi (modulo i n))
            (yi (modulo (quotient i n) n))
            (zi (quotient i (* n n))))
        (vector-set! points i
          (v3 (+ offset (* spacing xi))
              (+ offset (* spacing yi))
              (+ offset (* spacing zi))))))))

;;; --------------------------- THE SCENE --------------------------

;; Composed flow fields. Both are CONSTRUCTORS taking t: the closure
;; captures it, so per-frame rebuilding is an allocation here and would
;; be a uniform write on the GPU.
(define (flow t)
  (curl3 (fzoom 0.6 (wave t))
         (fzoom 0.6 (wave (+ t 17.0)))
         (fzoom 0.6 (wave (+ t 31.0)))))

;; A cheap (non-curl) vector field, 3 wave evaluations. Used only to
;; distort the domain -- warping does not need to be divergence-free.
(define (ripple t)
  (let ((w1 (wave t)) (w2 (wave (+ t 5.0))) (w3 (wave (+ t 9.0))))
    (lambda (p) (v3 (w1 p) (w2 p) (w3 p)))))

;; The same flow, sampled through a distorted domain. Pure composition,
;; no new machinery -- and warping with ripple rather than another
;; flow keeps this at 15 field evaluations instead of 24.
(define (warped-flow t)
  (warp (flow t) (fscale 0.5 (ripple (* 0.5 t)))))

;; 4^3 = 64 points. Deliberately modest: 12 finite-difference field
;; evaluations per point per frame is what an interpreter can afford at
;; 60fps. A compiler differentiating the field tree symbolically would
;; pay ~3, and this cube could be orders of magnitude denser.
(define cube (make-point-cube 4 1.8))
(define t 0.0)
(define dt 0.05)
(define frame 0)

;; switch routing FIELD CONSTRUCTORS. advect never learns which it got.
(define pick-field
  (switch (lambda () (modulo (quotient frame 180) 2)) flow warped-flow))

(define settle (contain 3.0 0.05))

;; Fixed isometric-ish view. Rotation constants hoisted out of the
;; per-point loop. No camera controls yet.
(define ry 0.6)
(define rx 0.4)
(define cos-ry (cos ry))
(define sin-ry (sin ry))
(define cos-rx (cos rx))
(define sin-rx (sin rx))

(define (draw-cube points)
  (let ((n (vector-length points))
        (cx (/ (canvas-width) 2))
        (cy (/ (canvas-height) 2)))
    (do ((i 0 (+ i 1)))
        ((= i n))
      (let* ((p (vector-ref points i))
             (x (v3x p)) (y (v3y p)) (z (v3z p))
             (x1 (+ (* x cos-ry) (* z sin-ry)))
             (z1 (+ (* (- x) sin-ry) (* z cos-ry)))
             (y2 (- (* y cos-rx) (* z1 sin-rx)))
             (z2 (+ (* y sin-rx) (* z1 cos-rx)))
             (s (/ 260.0 (+ 8.0 z2)))
             (b (max 0.15 (min 1.0 (/ s 40.0)))))
        (canvas-draw-circle (+ cx (* x1 s)) (+ cy (* y2 s))
                            (max 1.0 (* s 0.05))
                            (* 0.35 b) (* 0.8 b) 1.0 0.85)))))

(future
  (let loop ()
    (canvas-clear 0.02 0.03 0.05 1.0)
    ;; Rebuilt every frame: the field's params move, its structure does not.
    ((pipe (advect (pick-field t) dt) settle) cube)
    (draw-cube cube)
    (set! t (+ t dt))
    (set! frame (+ frame 1))
    (yield)
    (loop)))
`,

    attractor: `;;; ==========================================================
;;; Chaos Game / Barnsley Fern Attractor
;;; ==========================================================
;;; Computes iterated affine fractal transformations in a background
;;; fiber without ever stalling the browser UI!

(canvas-clear 0.02 0.03 0.05 1.0)

(define (draw-fern n-points)
  (future
    (let loop ((x 0.0) (y 0.0) (count 0))
      (let ((w (canvas-width))
            (h (canvas-height)))
        ;; Plot point in screen coords
        (let ((sx (+ (/ w 2) (* x 55)))
              (sy (- (- h 30) (* y 55))))
          (canvas-draw-circle sx sy 1.0 0.25 0.95 0.45 0.75))
        
        ;; Affine IFS transformations
        (let* ((r (random 100))
               (next-coords
                (cond
                  ((< r 1)  (list 0.0 (* 0.16 y)))
                  ((< r 86) (list (+ (* 0.85 x) (* 0.04 y))
                                  (+ (* -0.04 x) (* 0.85 y) 1.6)))
                  ((< r 93) (list (- (* 0.20 x) (* 0.26 y))
                                  (+ (* 0.23 x) (* 0.22 y) 1.6)))
                  (else     (list (+ (* -0.15 x) (* 0.28 y))
                                  (+ (* 0.26 x) (* 0.24 y) 0.44))))))
          
          ;; Yield every 50 points so we see it grow smoothly!
          (if (= (remainder count 50) 0)
              (yield))
          
          (if (< count n-points)
              (loop (car next-coords) (cadr next-coords) (+ count 1))
              'fern-complete))))))

(draw-fern 15000)
`,

    mcmc: `;;; ==========================================================
;;; 2D Metropolis-Hastings MCMC Target Sampler
;;; ==========================================================
;;; Live probabilistic programming: samples from a multi-modal target
;;; distribution and visualizes sample convergence live.

(canvas-clear 0.04 0.05 0.08 1.0)

;; Target Log-Density: Mixture of two 2D Gaussians
(define (target-log-p x y)
  (let* ((d1 (+ (* (- x 250) (- x 250)) (* (- y 250) (- y 250))))
         (d2 (+ (* (- x 550) (- x 550)) (* (- y 350) (- y 350))))
         (p1 (exp (/ (- d1) 6000.0)))
         (p2 (* 1.5 (exp (/ (- d2) 8000.0)))))
    (+ p1 p2)))

;; MCMC Sampling Fiber
(future
  (let loop ((curr-x 400.0) (curr-y 300.0) (samples 0) (accepted 0))
    ;; Propose step from Gaussian random walk
    (let* ((prop-x (+ curr-x (- (random 40.0) 20.0)))
           (prop-y (+ curr-y (- (random 40.0) 20.0)))
           (curr-p (target-log-p curr-x curr-y))
           (prop-p (target-log-p prop-x prop-y))
           (alpha (if (> curr-p 0) (/ prop-p curr-p) 1.0))
           (accept? (< (random 1.0) alpha))
           (nx (if accept? prop-x curr-x))
           (ny (if accept? prop-y curr-y)))
      
      ;; Draw sample point
      (if accept?
          (canvas-draw-circle nx ny 2.2 0.2 0.8 1.0 0.45)
          (canvas-draw-circle prop-x prop-y 1.0 0.9 0.2 0.3 0.15))
      
      ;; Yield every 5 samples for responsive 60fps rendering
      (if (= (remainder samples 5) 0)
          (yield))
      
      (loop nx ny (+ samples 1) (if accept? (+ accepted 1) accepted)))))
`,

    wave: `;;; ==========================================================
;;; Interactive Harmonic Wavefront Simulation
;;; ==========================================================
;;; Move your mouse across the canvas to interact with the wave generator!

(future
  (let loop ((t 0))
    ;; Fade background
    (canvas-fill-rect 0 0 (canvas-width) (canvas-height) 0.03 0.04 0.07 0.2)
    
    (let ((mx (mouse-x))
          (my (mouse-y)))
      ;; Draw 12 harmonic ripple rings
      (let ring-loop ((i 0))
        (when (< i 12)
          (let* ((phase (+ (* t 0.05) (* i 0.5)))
                 (radius (remainder (floor (* phase 40)) 350))
                 (alpha (- 1.0 (/ radius 350.0)))
                 (r (+ 0.3 (* 0.7 (sin phase))))
                 (g (+ 0.5 (* 0.5 (cos phase))))
                 (b 0.9))
            (canvas-draw-circle mx my radius r g b (* alpha 0.4))
            (ring-loop (+ i 1))))))
    
    (yield)
    (loop (+ t 1))))
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

  // Preset Selection Event
  selectPreset.addEventListener('change', () => {
    const val = selectPreset.value;
    if (PRESETS[val]) {
      if (vxsClearFibers) vxsClearFibers();
      ctx.fillStyle = '#05070a';
      ctx.fillRect(0, 0, canvas.width, canvas.height);
      editor.value = PRESETS[val];
      logToTerm(`\n--- Loaded preset: [${selectPreset.options[selectPreset.selectedIndex].text}] ---`, 'meta');
      executeSchemeCode();
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
    if (fibersRunning) {
      fibersToggleIcon.textContent = '⏸';
      fibersToggleText.textContent = 'Fibers Running';
      btnToggleFibers.style.borderColor = 'var(--accent-green)';
    } else {
      fibersToggleIcon.textContent = '▶';
      fibersToggleText.textContent = 'Fibers Paused';
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
  createVxsModule().then((Module) => {
    try {
      Module._vxs_init();
      vxsEval = Module.cwrap('vxs_eval', 'string', ['string']);
      vxsEvalJson = Module.cwrap('vxs_eval_json', 'string', ['string']);
      vxsStepFibers = Module.cwrap('vxs_step_fibers', 'number', []);
      vxsActiveFibersCount = Module.cwrap('vxs_active_fibers_count', 'number', []);
      vxsClearFibers = Module.cwrap('vxs_clear_fibers', null, []);
      vxsStatsJson = Module.cwrap('vxs_stats_json', 'string', []);

      isWasmReady = true;
      statusBadge.classList.add('active');
      statusText.textContent = 'Wasm Engine Ready';
      logToTerm('✓ WebAssembly NaN-Boxed Scheme Core initialized successfully (250 KB).', 'meta');
      logToTerm('✓ C++20 Fiber Coroutine Scheduler hooked into requestAnimationFrame (60 FPS).', 'meta');

      // Load initial preset
      editor.value = PRESETS.particles;
      executeSchemeCode();
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
