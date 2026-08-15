// testcases/test_wasm.js
const createVxSchemeModule = require('../web/vxs.js');

createVxSchemeModule().then((VXS) => {
  console.log('[1] Initializing Vx-Scheme Wasm Core...');
  const init = VXS._vxs_init();
  console.log('    vxs_init returned:', init);

  console.log('[2] Evaluating Arithmetic: (+ 123 456)...');
  const res1 = VXS.ccall('vxs_eval', 'string', ['string'], ['(+ 123 456)']);
  console.log('    Result:', res1);

  console.log('[3] Evaluating Future & Touch: (touch (future (* 6 7)))...');
  const res2 = VXS.ccall('vxs_eval', 'string', ['string'], ['(touch (future (* 6 7)))']);
  console.log('    Result:', res2);

  console.log('[4] Testing Cooperative Fibers and Stepping...');
  VXS.ccall('vxs_eval', 'string', ['string'], ['(define bg-val 0)']);
  VXS.ccall('vxs_eval', 'string', ['string'], ['(future (set! bg-val 1) (yield) (set! bg-val 2) (yield) (set! bg-val 3))']);
  
  const initialActive = VXS._vxs_active_fibers_count();
  console.log('    Active fibers initially:', initialActive);

  VXS._vxs_step_fibers();
  const val1 = VXS.ccall('vxs_eval', 'string', ['string'], ['bg-val']);
  console.log('    bg-val after step 1:', val1);

  VXS._vxs_step_fibers();
  const val2 = VXS.ccall('vxs_eval', 'string', ['string'], ['bg-val']);
  console.log('    bg-val after step 2:', val2);

  VXS._vxs_step_fibers();
  const val3 = VXS.ccall('vxs_eval', 'string', ['string'], ['bg-val']);
  console.log('    bg-val after step 3:', val3);

  const finalActive = VXS._vxs_active_fibers_count();
  console.log('    Active fibers at end:', finalActive);

  if (res1 === '579' && res2 === '42' && val1 === '1' && val2 === '2' && val3 === '3') {
    console.log('=== ALL WEBASSMEBLY CORE TESTS PASSED! ===');
    process.exit(0);
  } else {
    console.error('FAIL: Output mismatch!');
    process.exit(1);
  }
}).catch((err) => {
  console.error('Fatal Wasm Test Error:', err);
  process.exit(1);
});
