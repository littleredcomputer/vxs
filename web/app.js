// app.js: Vx-Scheme WebAssembly Creative Concurrency Workbench

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

  // Animation & Fiber Stepping Loop (60 FPS)
  function renderLoop(time) {
    // Compute FPS
    frameCount++;
    if (time - lastFrameTime >= 500) {
      currentFps = Math.round((frameCount * 1000) / (time - lastFrameTime));
      tagFps.textContent = `${currentFps} FPS`;
      frameCount = 0;
      lastFrameTime = time;
    }

    if (isWasmReady && fibersRunning) {
      try {
        const hasMore = vxsStepFibers(2500);
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

  // Wasm Initializer (createVxSchemeModule Promise)
  createVxSchemeModule().then((Module) => {
    try {
      Module._vxs_init();
      vxsEval = Module.cwrap('vxs_eval', 'string', ['string']);
      vxsEvalJson = Module.cwrap('vxs_eval_json', 'string', ['string']);
      vxsStepFibers = Module.cwrap('vxs_step_fibers', 'number', []);
      vxsActiveFibersCount = Module.cwrap('vxs_active_fibers_count', 'number', []);
      vxsClearFibers = Module.cwrap('vxs_clear_fibers', null, []);

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
