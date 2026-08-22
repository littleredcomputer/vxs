//----------------------------------------------------------------------
// GPU host paths, against a fake WebGPU.
//
// No shader ever runs here. What is under test is everything AROUND the
// shader — handle lifetimes, uploads, the readback staging dance, error
// settling, and whether a compile diagnostic reaches the page — and those
// are precisely the places where failures were silent.
//
// Two things this exists to keep honest, both from a field report:
//
//   A bad shader used to produce 60fps, no error, and a black canvas,
//   because createShaderModule does not throw and nothing read the
//   compilation info. Compilation is now a future, and the draw calls take
//   the handle it settles with, so nothing can reach a pipeline without
//   having waited for the compile. The fake reports compile messages the
//   way WebGPU does — asynchronously, after the call has returned — so the
//   test exercises the real timing rather than a convenient version of it.
//
//   Nothing could be read back off the device, so a GPU-resident program
//   had exactly zero measurable quantities. A round trip that returns the
//   bytes that were written is the smallest possible proof that a number
//   can now be got out.
//----------------------------------------------------------------------

const path = require('path');
const { installFakeWebGPU } = require(path.join(__dirname, 'fake_webgpu.js'));

// A shader carrying this marker is reported as failing to compile.
const POISON = 'BADSHADER';
const fakeDevice = installFakeWebGPU({
  compileMessages: (code) =>
    code.indexOf(POISON) >= 0
      ? [{ type: 'error', lineNum: 2, linePos: 4, message: 'no entry point found' },
         { type: 'warning', lineNum: 3, linePos: 1, message: 'unused binding' }]
      : [],
});

const createVxsModule = require(path.join(__dirname, '..', 'web', 'vxs.js'));

let bad = 0, total = 0;
const terminal = [];
function check(name, cond, detail) {
  total++;
  if (!cond) bad++;
  console.log(`  ${cond ? '✅ [PASS]' : '❌ [FAIL]'} ${name}${cond ? '' : '\n        got: ' + detail}`);
}

(async () => {
  globalThis.vxsPrint = (t) => terminal.push(t);
  const M = await createVxsModule();
  M.ccall('vxs_init', null, [], []);
  const ev = (s) => M.ccall('vxs_eval', 'string', ['string'], [s]).trim();
  // The fake resolves promises on later turns, as the real API does, so
  // pumping has to yield to the event loop between steps.
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

  //--- readback round trip ---------------------------------------------
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
  // reading everything is rarely what you want anyway.
  ev('(define narrowed #f)');
  ev('(future (set! narrowed (touch (gpu-buffer-read dev handle 84))))');
  await pump(15);
  check('an explicit length narrows the read',
        ev('(bytes-length (cdr narrowed))') === '84',
        ev('(bytes-length (cdr narrowed))'));

  //--- compilation is a future -----------------------------------------
  const GOOD = '@compute @workgroup_size(64)\\nfn main() {}\\n';
  const BAD  = `@compute @workgroup_size(64)\\nfn ${POISON}() {\\n  let x = 1;\\n}\\n`;

  terminal.length = 0;
  ev('(define good-sh #f)');
  ev(`(future (set! good-sh (touch (gpu-compile dev "${GOOD}"))))`);
  await pump(10);
  check('a good shader settles with a handle',
        ev('(handle? good-sh)') === '#t', ev('good-sh'));
  check('of kind :gpu-shader',
        ev('(handle-kind good-sh)') === ':gpu-shader', ev('(handle-kind good-sh)'));
  check('and says nothing', !/WGSL/.test(terminal.join('\n')), terminal.join('\n'));

  // The report's acceptance criteria: a message carrying line, column,
  // text and the source line must reach the PAGE, and the program must
  // stop rather than looping silently forever.
  // A failed compile CANNOT be caught with `guard`: touch has to suspend
  // the fiber, and guard's continuation contains native frames, so a touch
  // inside one is refused outright. That is a pre-existing rule of this
  // system, not something compilation introduces — every await in
  // lib/gpu.scm already sits outside any guard for the same reason.
  //
  // So the shape under test is the legal one: a bare await, and a bad
  // shader kills the fiber loudly with the compile message attached. The
  // statement after the touch must never run.
  terminal.length = 0;
  ev('(define bad-sh (quote untouched))');
  ev(`(future (set! bad-sh (touch (gpu-compile dev "${BAD}"))))`);
  await pump(10);

  const joined = terminal.join('\n');
  check('a bad shader reports its error to the page terminal',
        /WGSL error at line 2:4 — no entry point found/.test(joined), joined);
  check('with the offending source line',
        /2 \| fn BADSHADER\(\) \{/.test(joined), joined);
  check('warnings are reported too, and labelled as warnings',
        /WGSL warning at line 3:1 — unused binding/.test(joined), joined);
  check('the fiber dies at the touch, so nothing downstream of it runs',
        ev('bad-sh') === 'untouched', ev('bad-sh'));
  check('and the death report names the compile error, not a bare tag',
        /no entry point found/.test(joined), joined);

  //--- the draw path cannot be reached without one ----------------------
  ev('(define bypass #f)');
  ev(`(future (set! bypass
              (guard (e (#t 'raised)) (gpu-wrangle! dev handle "${GOOD}" 3 0.0 1))))`);
  await pump(5);
  check('passing SOURCE where a shader handle belongs raises',
        ev('bypass') === 'raised', ev('bypass'));

  ev('(define drew #f)');
  ev(`(future (set! drew
              (guard (e (#t (list 'raised (error-object-message e))))
                (begin (gpu-wrangle! dev handle good-sh 3 0.0 1) 'ok))))`);
  await pump(5);
  check('passing the compiled handle works', ev('drew') === 'ok', ev('drew'));

  //--- handle hygiene ---------------------------------------------------
  //--- live parameters cost no recompile -------------------------------
  // The whole point of the parameter block. A constant baked into the
  // shader SOURCE means moving a slider produces a new source string, a
  // new shader and a new pipeline, once per frame the slider moves — which
  // is the difference between a knob you demonstrate and a knob you play.
  // Here the values ride in the uniform, which is rewritten before every
  // dispatch anyway.
  ev(`(wrangle-params! '(sigma gain))`);
  ev('(define PB (make-wrangle-params)) (define PV (wrangle-params-view PB))');
  ev('(define wsh #f)');
  ev(`(future (set! wsh (touch (gpu-compile dev
        (wrangle-wgsl "pt_write(i, pt_pos(i), 1.0, vec3<f32>(w.p0, w.p1, 0.0));")))))`);
  await pump(8);
  check('a wrangle kernel compiles', ev('(handle? wsh)') === '#t', ev('wsh'));

  const compiles0 = fakeDevice._compiles || 0;
  const seen = [];
  for (const [sg, gn] of [[0.25, 1.0], [0.5, 2.0], [0.75, 3.0]]) {
    ev(`(param-set! PV 'sigma ${sg}) (param-set! PV 'gain ${gn})`);
    ev(`(gpu-wrangle! dev handle wsh 3 0.0 7 PB)`);
    const w = fakeDevice.queue._lastWrite;
    const f = new Float32Array(w.bytes.buffer, 0, 12);
    const u = new Uint32Array(w.bytes.buffer, 0, 12);
    seen.push({ size: w.size, seed: u[2], p0: f[4], p1: f[5] });
  }
  check('the uniform block is 48 bytes',
        seen.every((r) => r.size === 48), JSON.stringify(seen[0]));
  check('parameters reach the kernel by declared slot',
        seen[0].p0 === 0.25 && seen[0].p1 === 1 &&
        seen[2].p0 === 0.75 && seen[2].p1 === 3, JSON.stringify(seen));
  check('and changing them recompiles NOTHING',
        (fakeDevice._compiles || 0) - compiles0 === 0,
        `${(fakeDevice._compiles || 0) - compiles0} extra compiles`);

  // The seed used to be an f32 the shader converted with u32(), which
  // aliases every value past 2^24 onto its neighbours: "a different seed"
  // quietly meaning "the same noise".
  check('the seed arrives as a true u32',
        seen.every((r) => r.seed === 7), JSON.stringify(seen.map((r) => r.seed)));
  ev(`(gpu-wrangle! dev handle wsh 3 0.0 16777217)`);
  const tail = fakeDevice.queue._lastWrite.bytes.buffer;
  check('so a seed past 2^24 survives exactly',
        new Uint32Array(tail, 0, 12)[2] === 16777217,
        String(new Uint32Array(tail, 0, 12)[2]));
  check('and omitting the block leaves the slots zero',
        new Float32Array(tail, 0, 12)[4] === 0,
        String(new Float32Array(tail, 0, 12)[4]));

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
