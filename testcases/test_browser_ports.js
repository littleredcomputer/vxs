//----------------------------------------------------------------------
// Browser host semantics: where output goes, and how a submission is
// sequenced. Both are things the Scheme suite cannot see, because both are
// about the boundary between the VM and the page.
//
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

  // --- top-level form sequencing -------------------------------------
  // A submission is compiled and run ONE FORM AT A TIME, like `load` does
  // natively. It used to be read_all_forms()'d and compiled whole, which
  // breaks anything whose effect must land before the NEXT form compiles.
  // Macros are exactly that, and it made define-kernel unbound in the
  // browser while working natively.

  {
    const r = ev([
      '(load "lib/gpu.scm")',
      '(define-kernel k (vec3 (swizzle uv x) 0 0))',
      '(string? k)'
    ].join('\n'));
    check("a macro from a loaded lib is usable in the same submission",
          r.trim() === '#t', `got ${r}`);
  }

  {
    const r = ev([
      '(defmacro (twice x) `(* 2 ,x))',
      '(twice 21)'
    ].join('\n'));
    check("a macro defined and used in one submission",
          r.trim() === '42', `got ${r}`);
  }

  {
    const r = ev([
      '(define seq-a 1)',
      '(define seq-b (+ seq-a 1))',
      'seq-b'
    ].join('\n'));
    check("an earlier form's definition is visible to a later one",
          r.trim() === '2', `got ${r}`);
  }

  {
    // A failing form stops the submission rather than running on into
    // forms that assumed it succeeded.
    lines.length = 0;
    ev('(car (quote ()))\n(display "must not run")');
    check("a failing form halts the rest of the submission",
          !lines.join('').includes('must not run'), JSON.stringify(lines));
  }

  // --- malformed syntax must not abort the module ---------------------
  // compile_function used to assert(sym_val.is_symbol()) on parameter
  // lists built from SOURCE TEXT. Reachable from a program, an assert is
  // not a check but a crash: it aborted the whole wasm module, which in a
  // browser means a page reload. (do (i 0 (+ i 1)) ...) — one missing
  // layer of parentheses — was enough to trigger it.
  //
  // The load-bearing assertion here is the last one: the module is still
  // answering afterwards.
  {
    const r = ev("(do (i 0 (+ i 1)) ((= i 3) 'ok))");
    check("a malformed do reports rather than aborts",
          /each binding must be/.test(r), `got ${r}`);
    check("and says the bindings are a list of forms",
          /LIST of such forms/.test(r), `got ${r}`);
  }

  {
    const r = ev('((lambda (1) 2) 3)');
    check("a non-symbol parameter reports rather than aborts",
          /malformed parameter list/.test(r), `got ${r}`);
  }

  // Malformed BINDING names. All of let/let*/letrec funnel through
  // parse_bindings, so one check covers the three; define and set! take
  // names from elsewhere and are checked at their own sites. Each of these
  // aborted the module before, via Value::as_symbol_id()'s assert.
  {
    const cases = [
      ["(let ((1 2)) 3)",     /let: binding name must be a symbol/],
      ["(let* ((1 2)) 3)",    /let\*: binding name must be a symbol/],
      ["(letrec ((1 2)) 3)",  /letrec: binding name must be a symbol/],
      ["(define (1) 2)",      /define: procedure name must be a symbol/],
      ["(set!)",              /set!: expected \(set! name value\)/],
      ["(set! 5 1)",          /set!: target must be a symbol/],
      ["(make-vector -1)",    /make-vector: expected a non-negative length/],
    ];
    for (const [src, want] of cases) {
      const r = ev(src);
      check(`${src} reports rather than crashes`, want.test(r), `got ${r}`);
    }
  }

  {
    const r = ev('(+ 20 22)');
    check("the module still evaluates after a syntax error",
          r.trim() === '42', `got ${r}`);
  }

  // --- watch mode: runtime library overrides --------------------------
  // lib/*.scm is compiled into the binary, so a library edit is invisible
  // in the browser until a rebuild — that bit us once, when `if` in the
  // kernel language worked natively and did not exist in the page. Watch
  // mode serves the libraries over HTTP instead and registers them here;
  // `load` prefers a registered override over the baked-in copy.
  {
    const names = M.ccall('vxs_lib_names', 'string', [], []).split(',');
    check("the binary reports which libraries it carries",
          names.includes('wgsl.scm') && names.includes('prelude.scm'),
          JSON.stringify(names));
  }

  {
    const baked = ev('(begin (load "lib/wgsl.scm") (wgsl-code 1 (quote ())))');
    check("a library loads from the baked-in copy by default",
          baked.trim() === '"1.0"', `got ${baked}`);

    M.ccall('vxs_register_lib', null, ['string', 'string'],
            ['wgsl.scm', '(define (wgsl-code e env) "FROM-OVERRIDE")']);
    const over = ev('(begin (load "lib/wgsl.scm") (wgsl-code 1 (quote ())))');
    check("a registered override wins over the baked-in copy",
          over.trim() === '"FROM-OVERRIDE"', `got ${over}`);

    // Registering by basename must match however the path was spelled.
    const byPath = ev('(begin (load "wgsl.scm") (wgsl-code 1 (quote ())))');
    check("the override resolves by basename, not by path",
          byPath.trim() === '"FROM-OVERRIDE"', `got ${byPath}`);
  }

  console.log("\n────────────────────────────────────────────────────────────────");
  console.log(`Browser host: ${total} | Passed: ${total - bad} | Failed: ${bad}`);
  console.log(bad ? "❌ BROWSER HOST TESTS FAILED" : "✨ BROWSER HOST TESTS PASSED ✨");
  process.exit(bad ? 1 : 0);
})();
