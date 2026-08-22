//----------------------------------------------------------------------
// GPU host paths, against a fake WebGPU.
//
// No shader ever runs here. What is under test is everything AROUND the
// shader — handle lifetimes, buffer uploads, the readback staging dance,
// error settling — and those are precisely the places where failures have
// been silent.
//
// This exists because of one line in a field report: nothing could be read
// back off the device, so a GPU-resident program had exactly zero
// measurable quantities. A round trip that returns the bytes that were
// written is the smallest possible proof that a number can be got out.
//
// It also answers the reporter's other constraint — "I work without a
// browser, so a failure only reaches me if it becomes text someone can
// paste." Every path here used to require a canvas and a pair of eyes.
//----------------------------------------------------------------------

const path = require('path');
const { installFakeWebGPU } = require(path.join(__dirname, 'fake_webgpu.js'));

// Must run before the module loads: js_ensure_handle_table only builds a
// table if none exists, and navigator.gpu is read at adapter request.
installFakeWebGPU({ compileMessages: () => [] });

const createVxsModule = require(path.join(__dirname, '..', 'web', 'vxs.js'));

let bad = 0, total = 0;
function check(name, cond, detail) {
  total++;
  if (!cond) bad++;
  console.log(`  ${cond ? '✅ [PASS]' : '❌ [FAIL]'} ${name}${cond ? '' : '\n        got: ' + detail}`);
}

(async () => {
  globalThis.vxsPrint = () => {};
  const M = await createVxsModule();
  M.ccall('vxs_init', null, [], []);
  const ev = (s) => M.ccall('vxs_eval', 'string', ['string'], [s]).trim();
  // The fake resolves promises on later turns, as the real API does, so
  // pumping has to give the event loop a turn between steps.
  const pump = async (n) => {
    for (let i = 0; i < n; i++) {
      M.ccall('vxs_step_fibers', 'number', ['number'], [0]);
      await new Promise((r) => setTimeout(r, 1));
    }
  };

  ev('(load "lib/gpu.scm")');
  ev('(define dev #f) (define handle #f) (define result #f)');
  ev(`(future
        (let* ((a (touch (request-adapter)))
               (d (touch (request-device a)))
               (b (make-points 3))
               (v (points-view b)))
          (point-set! v 0 1.0 2.0 3.0 0.5 0.25 0.125 0.0625)
          (point-set! v 2 -1.0 -2.0 -3.0 0.5 0.0 0.0 0.0)
          (let ((h (gpu-buffer d b)))
            (set! dev d)
            (set! handle h)
            (set! result (touch (gpu-buffer-read d h))))))`);
  await pump(30);

  check('a readback settles', ev('(pair? result)') === '#t', ev('result'));
  check('stamped with the frame the copy was submitted on',
        parseInt(ev('(car result)'), 10) >= 0, ev('(car result)'));

  ev('(define rv (bytes-view (cdr result) :f32))');
  check('values written from Scheme come back unchanged',
        ev('(list (view-ref rv 0) (view-ref rv 1) (view-ref rv 2))') === '(1.0 2.0 3.0)',
        ev('(list (view-ref rv 0) (view-ref rv 1) (view-ref rv 2))'));
  check('including a later element, so the stride survived the trip',
        ev('(list (view-ref rv 14) (view-ref rv 15) (view-ref rv 16))') === '(-1.0 -2.0 -3.0)',
        ev('(list (view-ref rv 14) (view-ref rv 15) (view-ref rv 16))'));
  check('and an element never written is still zero',
        ev('(list (view-ref rv 7) (view-ref rv 8))') === '(0.0 0.0)',
        ev('(list (view-ref rv 7) (view-ref rv 8))'));

  // gpu-buffer rounds allocations up to 16 bytes, so a whole-buffer read
  // returns the padding too. That is what the length argument is for — and
  // reading everything is rarely what you want anyway: a full readback of
  // sixty thousand seven-float points is 1.7MB a probe.
  ev('(define narrowed #f)');
  ev('(future (set! narrowed (touch (gpu-buffer-read dev handle 84))))');
  await pump(15);
  check('an explicit length narrows the read',
        ev('(bytes-length (cdr narrowed))') === '84',
        ev('(bytes-length (cdr narrowed))'));

  ev('(define after-release #f)');
  ev(`(future (set! after-release
              (guard (e (#t 'raised)) (gpu-buffer-read dev 42))))`);
  await pump(5);
  check('reading through a non-handle raises',
        ev('after-release') === 'raised', ev('after-release'));

  console.log('\n────────────────────────────────────────────────────────────────');
  console.log(`GPU host paths: ${total} | Passed: ${total - bad} | Failed: ${bad}`);
  console.log(bad ? '❌ GPU HOST TESTS FAILED' : '✨ GPU HOST TESTS PASSED ✨');
  process.exit(bad ? 1 : 0);
})();
