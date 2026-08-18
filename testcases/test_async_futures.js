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
