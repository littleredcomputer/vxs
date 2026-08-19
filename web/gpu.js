// gpu.js — WebGPU proof of concept driven from vxs.
//
// The triangle is the boring part. What this actually demonstrates is that
// (touch (request-adapter)) suspends a Scheme fiber, the browser settles
// the promise from its own event loop, and the fiber resumes holding a
// handle to a real GPUAdapter — the external-future and handle machinery
// working against something that genuinely is asynchronous.
(function () {
  'use strict';

  var logEl = document.getElementById('log');
  var srcEl = document.getElementById('src');
  var btnRun = document.getElementById('btn-run');
  var btnClear = document.getElementById('btn-clear');

  function log(msg, cls) {
    var line = document.createElement('div');
    if (cls) line.className = cls;
    line.textContent = msg;
    logEl.appendChild(line);
  }
  function clear() { logEl.textContent = ''; }

  // Scheme's (display ...) lands here.
  window.vxsPrint = function (text) { if (text !== '') log(text); };

  // The shader. Written out in full rather than with the vec2f/vec4f
  // shorthands, which are newer aliases — the long forms work everywhere.
  var WGSL = [
    'struct VSOut {',
    '  @builtin(position) pos : vec4<f32>,',
    '  @location(0)       col : vec4<f32>,',
    '};',
    '',
    '@vertex',
    'fn vs(@builtin(vertex_index) i : u32) -> VSOut {',
    '  var pts = array<vec2<f32>, 3>(',
    '    vec2<f32>( 0.0,  0.6),',
    '    vec2<f32>(-0.6, -0.5),',
    '    vec2<f32>( 0.6, -0.5)',
    '  );',
    '  var cols = array<vec4<f32>, 3>(',
    '    vec4<f32>(1.0, 0.15, 0.25, 1.0),',
    '    vec4<f32>(0.2, 0.95, 0.45, 1.0),',
    '    vec4<f32>(0.25, 0.5, 1.0, 1.0)',
    '  );',
    '  var out : VSOut;',
    '  out.pos = vec4<f32>(pts[i], 0.0, 1.0);',
    '  out.col = cols[i];',
    '  return out;',
    '}',
    '',
    '@fragment',
    'fn fs(in : VSOut) -> @location(0) vec4<f32> {',
    '  return in.col;',
    '}'
  ].join('\n');

  // Note the shape: no callbacks, no await, no promise vocabulary at all.
  // `touch` blocks this fiber; the frame loop keeps running; the browser
  // settles the promise; the fiber picks up where it left off.
  // NOTE the shape: the awaits sit in the fiber body, NOT inside a guard.
  // `guard` runs its body through a native call, whose continuation
  // includes C++ frames — so a fiber inside one cannot suspend, and a
  // host-settled future can never be awaited there. Wrapping a GPU call in
  // an error handler is the obvious thing to write and is exactly what
  // does not work. Errors from an await still surface: a dying fiber now
  // reports itself to the console.
  //
  // gpu-draw-triangle! is synchronous, so it CAN be guarded.
  var SCHEME = [
    '(define wgsl *wgsl*)',
    '',
    '(future',
    '  (display "requesting adapter...") (newline)',
    '  (let ((adapter (touch (request-adapter))))',
    '    (display "  got ") (display (handle-kind adapter)) (newline)',
    '    (display "requesting device...") (newline)',
    '    (let ((device (touch (request-device adapter))))',
    '      (display "  got ") (display (handle-kind device)) (newline)',
    '      (guard (e (#t (display "draw failed: ")',
    '                    (display (if (error-object? e) (error-object-message e) e))',
    '                    (newline)))',
    '        (gpu-draw-triangle! device wgsl)',
    '        (display "submitted. triangle should be visible.") (newline)))))'
  ].join('\n');

  srcEl.textContent = SCHEME.replace('*wgsl*', '<the WGSL above>');

  var VXS = null;
  var ev = null;
  var step = null;
  var activeFibers = null;

  function run() {
    if (!VXS) { log('wasm not ready yet', 'err'); return; }
    clear();
    if (!navigator.gpu) {
      log('navigator.gpu is undefined — this browser has no WebGPU.', 'err');
      log('Safari: enable WebGPU in Develop > Feature Flags. Chrome/Edge 113+ work by default.', 'meta');
      // Run anyway: the failure should arrive as a catchable Scheme
      // condition rather than a hang, which is itself worth seeing.
    }
    // Give the Scheme its shader source as a string literal.
    var escaped = WGSL.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\n/g, '\\n');
    var program = SCHEME.replace('*wgsl*', '"' + escaped + '"');
    var res = ev(program);
    if (res && /error/i.test(res)) log(res, 'err');
  }

  btnRun.addEventListener('click', run);
  btnClear.addEventListener('click', clear);

  // The frame loop. Returning to the event loop between steps is what lets
  // a promise settle at all — a fiber awaiting the GPU can only progress
  // while the VM is not running.
  var statusEl = document.getElementById('status');
  var lastStatus = '';
  function frame() {
    if (VXS && step) {
      try { step(0); } catch (e) { log('step error: ' + e, 'err'); }
      if (statusEl) {
        // fibers > 0 with pending > 0 means "blocked, waiting on the host";
        // fibers > 0 with pending 0 means the settle landed but the fiber
        // did not resume, which would be our bug rather than the browser's.
        var s = 'fibers: ' + VXS._vxs_active_fibers_count() +
                '   pending externals: ' + VXS.cwrap('vxs_pending_externals', 'number', [])() +
                '   host handles: ' + VXS.cwrap('vxs_host_handle_count', 'number', [])();
        if (s !== lastStatus) { statusEl.textContent = s; lastStatus = s; }
      }
    }
    requestAnimationFrame(frame);
  }

  // Turn on the C++-side EM_JS tracing before init, so request-adapter
  // narrates itself to the console.
  globalThis.vxsDebugGpu = true;

  createVxsModule().then(function (M) {
    VXS = M;
    M._vxs_init();
    // Surface the module on globalThis so the settle path has a reachable
    // handle even if EM_JS's own `Module` binding is not what we expect —
    // and so it can be poked from the console.
    globalThis.vxsModule = M;
    ev = function (code) { return M.ccall('vxs_eval', 'string', ['string'], [code]); };
    step = M.cwrap('vxs_step_fibers', 'number', ['number']);
    activeFibers = M.cwrap('vxs_active_fibers_count', 'number', []);
    clear();
    log('vxs ready.', 'ok');
    log('navigator.gpu: ' + (navigator.gpu ? 'present' : 'ABSENT'),
        navigator.gpu ? 'ok' : 'err');
    log('(gpu-available?) => ' + ev('(gpu-available?)'), 'meta');
    log('Press Run.', 'meta');
    requestAnimationFrame(frame);
  }).catch(function (e) {
    clear();
    log('failed to load vxs.wasm: ' + e, 'err');
  });
})();
