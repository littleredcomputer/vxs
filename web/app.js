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

    points: `;;; ==========================================================
;;; GPU Instanced Points — Scheme fills a buffer, the GPU draws it
;;; ==========================================================
;;; A second harness. The plasma kernels draw ONE full-screen quad and
;;; compute a colour per pixel; this draws one small quad per POINT and
;;; reads each point's attributes from a storage buffer.
;;;
;;; The buffer is six floats per point — x, y, size, r, g, b — flat rather
;;; than an array of structs. WGSL gives vec3<f32> a 16-byte alignment
;;; inside a storage array, so a struct would not pack the way the same
;;; fields pack here, and the mismatch is silent: points simply appear in
;;; the wrong places.
;;;
;;; Scheme fills the buffer every frame. That is the point of this step:
;;; the path host -> buffer -> GPU works before any compute pass exists.
;;; Watch the obj/f and GC counters — this is the CPU cost that a compute
;;; wrangle will later delete.

(load "lib/gpu.scm")
(load "lib/threefry.scm")

(define N 2000)
(define buf (make-points N))
(define pts (points-view buf))

;;; Threefry doing what a counter-based RNG is for: randomness indexed by
;;; POINT NUMBER rather than drawn from a stream. No state, no ordering,
;;; and the same cloud every run.
;;;
;;; One block is FOUR independent 32-bit words and a point needs exactly
;;; four numbers, so this is one call per POINT — not one per attribute.
;;; The obvious-looking version calls threefry4x32 once per attribute and
;;; reads word 0 each time, throwing away three quarters of every block:
;;; four times the work for the same randomness. Measured at 3.9x.
(define key (vector 0 0 0 0))

(define radius   (make-vector N 0.0))
(define phase    (make-vector N 0.0))
(define speed    (make-vector N 0.0))
(define hue      (make-vector N 0.0))

(let fill ((i 0))
  (if (< i N)
      (let ((r (threefry4x32-unit (vector i 0 0 0) key)))
        (vector-set! radius i (* 0.85 (sqrt (vector-ref r 0))))
        (vector-set! phase  i (* 6.2831 (vector-ref r 1)))
        (vector-set! speed  i (+ 0.15 (* 0.9 (vector-ref r 2))))
        (vector-set! hue    i (vector-ref r 3))
        (fill (+ i 1)))))

(define (update! t)
  (let loop ((i 0))
    (if (< i N)
        (let* ((r (vector-ref radius i))
               (a (+ (vector-ref phase i) (* t (vector-ref speed i))))
               (h (vector-ref hue i))
               (wob (+ 1.0 (* 0.12 (sin (+ (* 3.0 a) t))))))
          (point-set! pts i
                      (* r wob (cos a))
                      (* r wob (sin a))
                      (+ 0.006 (* 0.012 h))
                      (+ 0.25 (* 0.75 h))
                      (+ 0.35 (* 0.45 (sin (+ a t))))
                      (+ 0.55 (* 0.45 (cos (* 0.7 t)))))
          (loop (+ i 1))))))

(run-points-loop buf N update! "vxs-gpu-canvas")
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
  const GPU_PRESETS = { plasma: true, rings: true, points: true };

  function showSurface(preset) {
    const wantsGpu = !!GPU_PRESETS[preset];
    canvas.style.display = wantsGpu ? 'none' : '';
    gpuCanvas.style.display = wantsGpu ? '' : 'none';
  }

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
    if (PRESETS[val]) {
      logToTerm(`\n--- Loaded preset: [${selectPreset.options[selectPreset.selectedIndex].text}] ---`, 'meta');
      loadPreset(val);
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
      vxsStepFibers = Module.cwrap('vxs_step_fibers', 'number', ['number']);
      vxsActiveFibersCount = Module.cwrap('vxs_active_fibers_count', 'number', []);
      vxsClearFibers = Module.cwrap('vxs_clear_fibers', null, []);
      vxsStatsJson = Module.cwrap('vxs_stats_json', 'string', []);

      isWasmReady = true;
      statusBadge.classList.add('active');
      statusText.textContent = 'Wasm Engine Ready';
      logToTerm('✓ WebAssembly NaN-Boxed Scheme Core initialized successfully (250 KB).', 'meta');
      logToTerm('✓ C++20 Fiber Coroutine Scheduler hooked into requestAnimationFrame (60 FPS).', 'meta');

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
