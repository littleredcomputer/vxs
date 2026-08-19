// End-to-end check of the external-future path: a Scheme fiber blocks on
// a future that only JS can settle, the event loop settles it, and the
// fiber resumes. setTimeout stands in for the promise-returning WebGPU
// calls this exists for — the mechanism is identical, only the source of
// the settle differs.
//
// The pump loop below is what requestAnimationFrame does in the browser:
// step fibers, return to the event loop, repeat. Returning is the whole
// point — a fiber awaiting the outside world can only make progress while
// the VM is NOT running.
const path = require('path');
const createVxsModule = require('../web/vxs.js');

let passed = 0, failed = 0;
function chk(name, expected, actual) {
  if (String(expected) === String(actual)) {
    console.log(`  ✅ [PASS] ${name}`);
    passed++;
  } else {
    console.log(`  ❌ [FAIL] ${name}\n     expected ${expected}, got ${actual}`);
    failed++;
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async () => {
  const VXS = await createVxsModule();
  VXS._vxs_init();
  const ev = (code) => VXS.ccall('vxs_eval', 'string', ['string'], [code]);
  const step = VXS.cwrap('vxs_step_fibers', 'number', ['number']);
  const activeFibers = VXS.cwrap('vxs_active_fibers_count', 'number', []);
  const pendingExternals = VXS.cwrap('vxs_pending_externals', 'number', []);
  const settleNumber = VXS.cwrap('vxs_settle_number', 'number', ['number', 'number']);

  // Drive like a frame loop: step, yield to the event loop, repeat.
  async function pump(maxMs = 3000) {
    const t0 = Date.now();
    while (Date.now() - t0 < maxMs) {
      step(0);
      if (activeFibers() === 0) break;
      await sleep(4);
    }
    return Date.now() - t0;
  }

  console.log('=== EXTERNAL FUTURES (async settled from JS) ===');

  // 1. A fiber awaits a timer and resumes with its value.
  ev(`(define woke 'no) (future (set! woke (touch (sleep 40))))`);
  // (future ...) spawns the fiber but does not run its body, so `sleep`
  // has not been called yet — the timer starts on the first step.
  chk('nothing pending until the fiber runs', 0, pendingExternals());
  step(0);
  chk('a pending external future is registered once it runs', 1, pendingExternals());
  chk('the fiber is blocked, not finished', 1, activeFibers());
  const elapsed = await pump();
  chk('fiber resumed with the settled value', '40.0', ev('woke'));
  chk('the pending entry was released', 0, pendingExternals());
  chk('waiting actually took time', true, elapsed >= 35);

  // 2. Waiting must not spin: the fiber is suspended, not burning budget.
  //    A 120ms sleep across ~4ms pumps should cost very few instructions,
  //    so total allocation stays flat while blocked.
  ev(`(define done2 #f) (future (touch (sleep 120)) (set! done2 #t))`);
  const statsBefore = JSON.parse(VXS.ccall('vxs_stats_json', 'string', [], []));
  await pump();
  const statsAfter = JSON.parse(VXS.ccall('vxs_stats_json', 'string', [], []));
  chk('fiber completed after the longer wait', '#t', ev('done2'));
  chk('blocking allocated almost nothing', true,
      statsAfter.total_objects_allocated - statsBefore.total_objects_allocated < 500);

  // 3. Several fibers awaiting independently, settling out of order.
  ev(`(define order '())
      (define (note x) (set! order (cons x order)))
      (future (touch (sleep 90)) (note 'slow))
      (future (touch (sleep 20)) (note 'fast))`);
  await pump();
  chk('independent waits settle in time order', '(slow fast)', ev('order'));

  // 4. A rejected/failed external future raises, catchable with guard.
  ev(`(define caught 'none)
      (define f (sleep 1000000))
      (future (set! caught (guard (e (#t 'caught)) (touch f))))`);
  // Reach in and fail it the way a rejected promise would.
  const tokenSettled = (() => {
    // token values are opaque; settle every plausible one until one takes
    for (let t = 1; t < 50; t++) {
      if (VXS.ccall('vxs_settle_error', 'number', ['number', 'string'], [t, 'boom'])) return t;
    }
    return 0;
  })();
  chk('settling as an error found the pending future', true, tokenSettled > 0);
  await pump();
  chk('a failed external future raises, and guard catches it', 'caught', ev('caught'));

  // 5. An unknown token is a quiet no-op, not a crash. This happens
  //    routinely: callbacks outlive their futures on teardown.
  chk('settling an unknown token is a no-op', 0, settleNumber(999999, 1));

  // 6. Teardown drops pending roots; late callbacks find nothing.
  ev(`(future (touch (sleep 100000)))`);
  step(0);   // run the fiber far enough to actually start the timer
  chk('a long wait is pending before teardown', true, pendingExternals() > 0);
  VXS.cwrap('vxs_clear_fibers', null, [])();
  chk('clear-fibers releases pending externals', 0, pendingExternals());
  chk('clear-fibers releases fibers', 0, activeFibers());

  // 7. Awaiting an external future from a SYNCHRONOUS eval is impossible
  //    (the event loop cannot run) and must say so rather than hang.
  const syncAttempt = ev(`(touch (sleep 50))`);
  chk('synchronous await reports guidance, not a hang', true,
      /cannot await an external future/.test(syncAttempt));

  // ---- handles to host objects -------------------------------------
  // A GPUDevice cannot cross into wasm, so the host keeps it in a table
  // and Scheme holds an opaque index. Exercised here with a plain JS
  // object; a real GPU resource takes the identical path.
  console.log('\n=== HOST HANDLES ===');

  const settleHandle = VXS.cwrap('vxs_settle_handle', 'number',
                                 ['number', 'number', 'string']);
  const hostHandleCount = VXS.cwrap('vxs_host_handle_count', 'number', []);

  const before = hostHandleCount();
  const fakeDevice = { name: 'fake-device', destroyed: false };
  const id = globalThis.vxsHandles.put(fakeDevice);
  chk('host table holds the object', before + 1, hostHandleCount());

  // Start from a clean slate so exactly one token is outstanding and the
  // scan below is unambiguous. (Tokens are internal — a real binding
  // never exposes them, because the primitive that starts the promise
  // also hands the token to JS itself.)
  VXS.cwrap('vxs_clear_fibers', null, [])();
  ev(`(define dev 'none) (define f2 (sleep 1000000))
      (future (set! dev (touch f2)))`);
  step(0);
  chk('exactly one external is outstanding', 1, pendingExternals());

  // Settle it with the handle rather than letting the timer win.
  let handled = 0;
  for (let t = 1; t < 500 && !handled; t++) handled = settleHandle(t, id, 'gpu-device');
  chk('settled a future with a handle', 1, handled);
  await pump();

  chk('Scheme received a handle',    '#t',         ev('(handle? dev)'));
  // handle-kind is a keyword, like every other tag we define — so
  // (eq? (handle-kind d) :gpu-device) holds without quoting.
  chk('the handle carries its kind', ':gpu-device', ev('(handle-kind dev)'));
  chk('the kind is eq? to the keyword literal', '#t',
      ev('(eq? (handle-kind dev) :gpu-device)'));
  chk('a fresh handle is not released', '#f',      ev('(handle-released? dev)'));

  // Releasing marks the OBJECT: every alias sees it, which is why a
  // handle is a heap object rather than a copied integer.
  ev('(define alias dev)');
  chk('release reports it did something', '#t', ev('(handle-release! dev)'));
  chk('the alias sees the release too',   '#t', ev('(handle-released? alias)'));
  chk('releasing twice is idempotent',    '#f', ev('(handle-release! dev)'));
  chk('host table dropped the object',    before, hostHandleCount());
  chk('the JS object itself survives (we only dropped our hold)',
      'fake-device', fakeDevice.name);

  // ---- awaiting from inside a native call ---------------------------
  // guard/map/apply/for-each/load run their body through call_closure, so
  // the fiber's continuation includes C++ frames and it cannot suspend.
  // A host-settled future can therefore never be awaited there — the event
  // loop would have to run, and we cannot return to it.
  //
  // The earlier "guard catches a failed future" test passes only because it
  // settles BEFORE pumping, so touch takes the already-completed path. This
  // is the case it misses, and the one that broke the first GPU page.
  console.log('\n=== AWAIT INSIDE A NATIVE CALL ===');

  VXS.cwrap('vxs_clear_fibers', null, [])();
  ev(`(define g 'pending)
      (future (set! g (guard (e (#t 'caught)) (touch (sleep 20)))))`);
  await pump(600);
  chk('a pending host-settled future cannot be awaited inside guard',
      'pending', ev('g'));
  chk('and the fiber does not survive it', 0, VXS._vxs_active_fibers_count());

  // The same await, moved out of the guard into the fiber body, works.
  VXS.cwrap('vxs_clear_fibers', null, [])();
  ev(`(define ok 'no) (future (set! ok (touch (sleep 20))))`);
  await pump(600);
  chk('the same await in the fiber body succeeds', '20.0', ev('ok'));

  // ---- touch/or-error: awaiting failure without a handler -----------
  // The interim answer to the guard limitation above. A raise needs a
  // handler; guard cannot suspend; and the failures that matter —
  // pipeline compilation, mapAsync, device-lost — are precisely the
  // asynchronous ones. Carrying failure as a VALUE needs no handler, so
  // it works across a suspension point.
  console.log('\n=== TOUCH/OR-ERROR ===');

  VXS.cwrap('vxs_clear_fibers', null, [])();
  ev(`(define ok2 'no) (future (set! ok2 (touch/or-error (sleep 20))))`);
  await pump(600);
  chk('on success it is just the value', '20.0', ev('ok2'));

  VXS.cwrap('vxs_clear_fibers', null, [])();
  ev(`(define bad 'no) (define lost (sleep 100000))
      (future (set! bad (touch/or-error lost)))`);
  step(0);
  let settledErr = 0;
  for (let t = 1; t < 500 && !settledErr; t++) {
    settledErr = VXS.ccall('vxs_settle_error', 'number', ['number', 'string'],
                           [t, 'device lost']);
  }
  await pump(600);
  chk('an async failure comes back as an error object', '#t',
      ev('(error-object? bad)'));
  chk('carrying its message', '"device lost"', ev('(error-object-message bad)'));
  chk('and the fiber survived to see it', 0, VXS._vxs_active_fibers_count());

  // The point: this is the case guard cannot express.
  VXS.cwrap('vxs_clear_fibers', null, [])();
  ev(`(define via 'no)
      (future (let ((r (touch/or-error (sleep 20))))
                (set! via (if (error-object? r) 'failed 'succeeded))))`);
  await pump(600);
  chk('branching on the outcome needs no handler at all', 'succeeded', ev('via'));

  // ---- WebGPU, on a host that has none ------------------------------
  // Node has no navigator.gpu, so this exercises exactly the half of the
  // GPU path that is verifiable headless: a real primitive, a real
  // external future, a real rejection, and the fiber resuming into a
  // catchable condition rather than hanging forever. The success half
  // needs a browser and lives on the page.
  console.log('\n=== WEBGPU (absent host) ===');

  chk('gpu-available? is #f without navigator.gpu', '#f', ev('(gpu-available?)'));

  ev(`(define outcome 'pending)
      (future (set! outcome
                (guard (e (#t (list 'caught (error-object-message e))))
                  (touch (request-adapter)))))`);
  await pump();
  chk('a rejected adapter request is caught, not hung',
      '(caught "WebGPU unavailable: navigator.gpu is undefined")', ev('outcome'));
  chk('no fiber is left blocked', 0, VXS._vxs_active_fibers_count());
  chk('the failed request released its pending entry', 0, pendingExternals());

  // Contract checks on the GPU primitives are ordinary raises.
  chk('request-device rejects a non-handle', 'caught',
      ev(`(guard (e (#t 'caught)) (request-device 42))`));
  chk('gpu-draw-triangle! rejects a non-handle', 'caught',
      ev(`(guard (e (#t 'caught)) (gpu-draw-triangle! 42 "wgsl"))`));

  console.log('\n────────────────────────────────────────────────────────────────');
  console.log(`External futures: ${passed + failed} | Passed: ${passed} | Failed: ${failed}`);
  if (failed === 0) {
    console.log('✨ EXTERNAL FUTURE TESTS PASSED ✨');
  } else {
    console.log('💥 EXTERNAL FUTURE TESTS FAILED');
  }
  // Exit explicitly. Clearing fibers drops the VM's side of a pending
  // external future, but the JS timer that would have settled it is still
  // scheduled — and a live timer keeps node's event loop (and therefore
  // `make test`) alive until it fires. That is correct behaviour for the
  // browser, where the page owns the loop; here it just means a test
  // harness must not rely on falling off the end to exit.
  process.exit(failed === 0 ? 0 : 1);
})().catch((e) => {
  console.error('FATAL:', e);
  process.exit(1);
});
