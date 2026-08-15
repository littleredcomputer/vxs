const fs = require('fs');
const path = require('path');
const createVxSchemeModule = require('../web/vxs.js');
const { runTestSuite } = require('../web/test_suite.js');

async function main() {
  const wasmPath = path.join(__dirname, '../web/vxs.wasm');
  const wasmBuffer = fs.readFileSync(wasmPath);

  const M = await createVxSchemeModule({
    wasmBinary: wasmBuffer
  });
  M._vxs_init();

  const vxsEvalJson = M.cwrap('vxs_eval_json', 'string', ['string']);
  const vxsClearFibers = M.cwrap('vxs_clear_fibers', null, []);

  console.log('╔════════════════════════════════════════════════════════════════╗');
  console.log('║   VX-SCHEME WEBASSEMBLY STRUCTURED UNIT TEST MATRIX RUNNER     ║');
  console.log('╚════════════════════════════════════════════════════════════════╝\n');

  const summary = await runTestSuite(vxsEvalJson, vxsClearFibers);

  for (const r of summary.results) {
    const mark = r.passed ? '✅ [PASS]' : '❌ [FAIL]';
    console.log(`${mark} ${r.name.padEnd(32)} (${r.elapsedMs} ms)`);
    if (!r.passed) {
      console.log(`    Reason: ${r.failureReason}`);
      console.log(`    Code:   ${r.code}`);
      console.log(`    Response:`, r.resObj);
    }
  }

  console.log('\n────────────────────────────────────────────────────────────────');
  console.log(`Total Tests: ${summary.total} | Passed: ${summary.passed} | Failed: ${summary.failed}`);
  if (summary.failed === 0) {
    console.log('✨ ALL STRUCTURED UNIT TESTS PASSED WITH 100% SUCCESS! ✨\n');
    process.exit(0);
  } else {
    console.error(`💥 ${summary.failed} TESTS FAILED!`);
    process.exit(1);
  }
}

main();
