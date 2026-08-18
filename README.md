# vxs

A small, fast, NaN-boxed Scheme implementation with a single-pass
bytecode compiler and a cooperative fiber scheduler — built to run
natively or as a compact WebAssembly module in the browser.

It's mostly R4RS: read the standard docs, write ordinary Scheme code,
and it should work. A handful of deviations are deliberate, not bugs —
see [Deviations from R4RS](#deviations-from-r4rs) below.

## Why

Scheme's roots are in SICP; this implementation's are too. The original
version of this codebase was written in 2002 for an embedded real-time
OS (VxWorks), under real memory constraints — a hand-tuned mark-and-sweep
collector, a from-scratch compiler, and full re-entrant `call/cc`, all
fitting in 256 KB. This 2026 rewrite keeps that lineage's taste for
compactness while retargeting it: 64-bit NaN-boxed values instead of a
heap-allocated variant type, a chunked/deque-backed VM stack instead of
a reallocating one, and WebAssembly as a first-class target instead of
an afterthought.

## Features

- **NaN-boxed values** — every Scheme value (fixnums, flonums, pairs,
  vectors, closures, ports, ...) fits in a single 8-byte word.
- **Single-pass bytecode compiler** — no bootstrap step, compiles at
  native speed.
- **Cooperative fiber scheduler** — quantum-stepped fibers driven by
  `requestAnimationFrame` in the browser, with `future`/`touch` for
  synchronization. Good for real-time simulations without blocking the
  UI thread.
- **Modern collection literals** — `[1 2 3]` vectors, `{:k v}` maps,
  bracketed `let`/`do` bindings, callable collections (`(vec i)`,
  `(:key map)`).
- **Procedural `defmacro`** with auto-gensym hygiene and `macroexpand`.
- **AOT compilation** — `vx-scheme --compile script.scm -o binary`
  produces a standalone native executable.
- **Runs the same engine natively and in WASM** — the WebAssembly build
  is a few hundred KB, not megabytes.

## Quick start

```sh
make            # builds the native binary and the WASM module
make test       # runs the full test battery (ground-up suite, GC
                # stress tests, structured unit tests, WASM presets,
                # classic benchmarks, AOT compiler smoke test)
```

```sh
./src/vx-scheme                    # REPL
./src/vx-scheme script.scm         # run a script
./src/vx-scheme -c script.scm -o out   # compile to a standalone binary
```

The in-browser workbench (Monaco editor, live canvas, six demo
presets) is plain static files — no build step:

```sh
cd web && python3 -m http.server
# then open http://localhost:8000
```

## Embedding from JavaScript

The WASM module runs under Node as readily as in a browser. The whole of
a minimal setup:

```js
const createVxsModule = require('./web/vxs.js');

createVxsModule().then((VXS) => {
  VXS._vxs_init();
  const ev = (code) => VXS.ccall('vxs_eval', 'string', ['string'], [code]);

  console.log(ev('(+ 1 2 3)'));                              // 6
  console.log(ev('(map (lambda (x) (* x x)) (list 1 2 3))')); // (1 4 9)
});
```

No canvas stubs are needed unless your Scheme actually draws — the
drawing hooks are optional and check for their host functions first.

**Fibers need someone to step them.** `(future ...)` creates a fiber but
does not run it. From inside Scheme, `(run-fibers)` drives them to
completion. From the host — which is what a frame loop does — call
`vxs_step_fibers(0)` until no fibers remain:

```js
const step = VXS.cwrap('vxs_step_fibers', 'number', ['number']);
while (VXS._vxs_active_fibers_count() > 0) step(0);
```

**Anything asynchronous needs the host to return to the event loop.** A
fiber blocked on a promise-backed future — `(touch (sleep 30))`, and in
time the promise-returning GPU calls — can only make progress while the
VM is *not* running. So the driving loop must yield between steps:

```js
while (VXS._vxs_active_fibers_count() > 0) {
  step(0);
  await new Promise((r) => setTimeout(r, 4));   // give the loop a turn
}
```

In a browser that `await` is simply `requestAnimationFrame`. Awaiting
such a future from a synchronous `vxs_eval` cannot work for the same
reason, and says so rather than stalling.

## Deviations from R4RS

A few places where this dialect intentionally departs from the
standard, along with why:

- **Symbols are case-sensitive.** R4RS technically mandates case
  folding; the wider Scheme (and Lisp-family) world has largely moved
  past it. Case-sensitive matches where practice actually is.
- **Vectors and maps are callable procedures** (`(vec i)`, `(:key map)`
  or `(map :key)`) — a deliberate Clojure-style ergonomic choice. This
  means `procedure?` and `vector?`/`map?` aren't strictly disjoint the
  way R4RS assumes.
- **`call/cc` is escape-only.** General re-entrant continuations were
  traded for a chunked, non-reallocating VM stack and the performance
  that comes with it — but the common "jump out of a loop early" use
  case is fully supported (implemented as a single-shot, upward-only
  unwind — see `ContinuationEscape` in `src/vx_vm.h`). Capturing a
  continuation and invoking it again later, from outside the `call/cc`
  that created it, fails with a runtime error instead of resuming.

## Project layout

```
src/            C++20 engine: NaN-boxed values, reader, compiler, VM, fiber scheduler
web/            Static in-browser workbench (Monaco editor, live canvas, demo presets)
testcases/      Ground-up test suite, r4rstest.scm, classic Scheme benchmarks
```

## License

MIT — see [LICENSE](LICENSE).
