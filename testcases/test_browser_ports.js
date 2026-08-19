//----------------------------------------------------------------------
// Browser output ports.
//
// The wasm build used to OVERRIDE display/newline to shove text at the JS
// terminal, which meant they ignored their port argument: (display x port)
// wrote to the terminal and printed the port object, and string ports did
// not work in the browser at all. The fix was to stop overriding the
// procedure and give the default PORT somewhere to lead — stdout is a sink
// port whose streambuf forwards complete lines to a named JS sink.
//
// These tests exist because none of that is visible from the Scheme suite:
// it needs a real wasm module with JS sinks attached to observe where the
// text actually lands.
//----------------------------------------------------------------------

const createVxsModule = require(require('path').join(__dirname, '..', 'web', 'vxs.js'));
(async () => {
  const lines = [];
  const consoleLines = [];
  const divLines = [];
  globalThis.vxsPrint = (t) => lines.push(t);
  globalThis.vxsSinks = { console: (t) => consoleLines.push(t), mydiv: (t) => divLines.push(t) };
  const M = await createVxsModule();
  M.ccall('vxs_init', null, [], []);
  const ev = (s) => M.ccall('vxs_eval', 'string', ['string'], [s]);

  let bad = 0, total = 0;
  const check = (name, cond, detail) => {
    total++;
    if (!cond) bad++;
    console.log(`  ${cond ? "✅ [PASS]" : "❌ [FAIL]"} ${name}${cond ? "" : "  " + detail}`);
  };

  // 1. string ports now work in the browser
  let r = ev('(with-output-to-string (lambda () (display "hi") (display 42)))');
  check("with-output-to-string captures", r.trim() === '"hi42"', `got ${r}`);
  r = ev('(call-with-output-string (lambda (p) (display (list 1 2) p)))');
  check("call-with-output-string captures", r.trim() === '"(1 2)"', `got ${r}`);

  // 2. display to terminal, buffered by LINE not by call
  lines.length = 0;
  ev('(begin (display "x = ") (display 42) (newline))');
  check("a line is assembled from several displays", lines.length === 1 && lines[0] === "x = 42",
        JSON.stringify(lines));

  // 3. explicit console port
  consoleLines.length = 0; lines.length = 0;
  ev('(begin (display "to the console" console-port) (newline console-port))');
  check("console-port routes to the console sink", consoleLines.length === 1 && consoleLines[0] === "to the console",
        JSON.stringify(consoleLines));
  check("and not to the terminal", lines.length === 0, JSON.stringify(lines));

  // 4. a page-registered sink (the div case)
  divLines.length = 0;
  ev('(let ((p (open-output-sink "mydiv"))) (display "into a div" p) (newline p))');
  check("open-output-sink reaches a registered handler",
        divLines.length === 1 && divLines[0] === "into a div", JSON.stringify(divLines));

  // 5. current-output-port rebinding
  lines.length = 0;
  r = ev('(with-output-to-string (lambda () (display "captured")))');
  check("rebinding steals output from the terminal", lines.length === 0, JSON.stringify(lines));

  // 6. trailing partial line is flushed at end of eval
  lines.length = 0;
  ev('(display "no newline here")');
  check("a trailing partial line is flushed", lines.length === 1 && lines[0] === "no newline here",
        JSON.stringify(lines));

  // 7. the port is a real port
  r = ev('(list (output-port? console-port) (output-port? terminal-port))');
  check("sink ports are output ports", r.trim() === '(#t #t)', `got ${r}`);

  console.log("\n────────────────────────────────────────────────────────────────");
  console.log(`Browser ports: ${total} | Passed: ${total - bad} | Failed: ${bad}`);
  console.log(bad ? "❌ BROWSER PORT TESTS FAILED" : "✨ BROWSER PORT TESTS PASSED ✨");
  process.exit(bad ? 1 : 0);
})();
