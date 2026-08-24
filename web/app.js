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
  // Demos live in demos/*.scm and are FETCHED, not embedded.
  //
  // They used to be template literals in this file, which cost more than
  // it looked: a backtick anywhere in the Scheme — and a backtick is
  // ordinary punctuation in prose, as well as quasiquote — closed the
  // literal and took the whole application with it. That happened. It also
  // meant the test harness had to recover preset text by matching markers
  // in this file, so it was never reading quite what the browser ran.
  //
  // As files they are editable without touching JavaScript, watchable by
  // the same machinery that already watches a scratch file, loadable by
  // the harness directly, and free to use quasiquote.
  const PRESET_NAMES = [
    'plasma', 'rings', 'ensemble', 'field', 'cubes',
    'actors', 'wrangle', 'points', 'fibers', 'repl'
  ];
  const presetCache = Object.create(null);

  async function fetchPreset(name) {
    if (presetCache[name]) return presetCache[name];
    const url = 'demos/' + name + '.scm';
    const res = await fetch(url, { cache: 'no-store' });
    if (!res.ok) throw new Error(`${res.status} ${res.statusText} for ${url}`);
    const text = await res.text();
    presetCache[name] = text;
    return text;
  }

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
  async function loadPreset(name) {
    if (PRESET_NAMES.indexOf(name) < 0) return;
    if (vxsClearFibers) vxsClearFibers();
    showSurface(name);
    ctx.fillStyle = '#05070a';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    try {
      editor.value = await fetchPreset(name);
    } catch (e) {
      logToTerm(`cannot load demos/${name}.scm — ${e.message}. ` +
                `Serve from the repo root ("python3 serve.py"), not from web/.`, 'err');
      return;
    }
    executeSchemeCode();
  }

  // Preset Selection Event
  selectPreset.addEventListener('change', () => {
    const val = selectPreset.value;
    if (PRESET_NAMES.indexOf(val) < 0) {
      // Was a silent no-op, which is the worst possible answer: selecting
      // a preset appeared to do nothing at all, with nothing in the log to
      // explain it. It means index.html and app.js have drifted — the
      // dropdown offers an option this script has never heard of.
      logToTerm(`No preset named "${val}" in this script — index.html and ` +
                `app.js have drifted, almost certainly because one came ` +
                `from cache. Serve with "python3 serve.py" from the repo ` +
                `root, which disables caching.`, 'err');
      return;
    }
    logToTerm(`\n--- Loaded preset: [${selectPreset.options[selectPreset.selectedIndex].text}] ---`, 'meta');
    loadPreset(val);
  });

  // Same drift, caught at startup rather than on the click: every option in
  // the markup should name a preset this script defines.
  Array.from(selectPreset.options).forEach((opt) => {
    if (PRESET_NAMES.indexOf(opt.value) < 0) {
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
