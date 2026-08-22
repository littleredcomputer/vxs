# vxs

Once upon a time (2002) after reading SICP I decided to write a
Scheme compiler in C++ based on the model in Chapter 5. After
many late nights, I had it passing an R4RS test suite. It was
stackless; everything about the execution was on the heeap, making
`call-with-current-continuation` almost trivial. It knew how to
call functions in the VxWorks shell (hence its name), could run
well in less than 1Mb, and had a Knuth garbage collector. It could
emit "standalone" executables, with nothing within but a table
of bytecode and the VM loop. I was proud of it.

24 years later, I decided to enlist Claude's aid to modernize it,
updating to C++20. We replaced the abuse of pointer lower bits
with NaN-boxing, and leveraged co-routines to give us a resumable
virtual machine. This allowed us to implement `future` and `yield`,
a sort of async/await for Scheme, that was portable via Emscripten
to WASM. With that, we acquire the capability to interface with
requestAnimationFrame and WebGPU.

It's mostly R4RS: read the standard docs, write ordinary Scheme code,
and it should work. A handful of deviations are deliberate, not bugs —
see [Deviations from R4RS](#deviations-from-r4rs) below.

We also tossed in a few UX improvements from Clojure while we were
at it, and have begun the process of building out an interface
between Scheme and WGSL that has some of the flavor of Shadertoy.

Many of the design decisions made in 2002 have paid off today.
The stackless design made the browser/JS async integration possible.
The compiler allows hot loops to iterate without allocating much
memory; tail calls and named lets are optimized for this. C++ turned
out to allow easy generation of WASM with zero dependencies: there's
no CMake or Meson here; just compile it and go.

## the manual

[MANUAL.md](MANUAL.md) covers the parts of vxs you cannot infer from the
source: where a fiber may suspend and where it may not, what `guard` does
and does not catch, and how the GPU pipeline fits together. If something
died and you are not sure why, start there. It ends with the known gaps and
the open work, so a rule and its exceptions stay next to each other.

## features

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

## quick start

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

The in-browser workbench is plain static files — no build step. Serve
from the REPO ROOT rather than `web/`, so that `lib/` and `demos/` are
reachable alongside the page:

```sh
python3 serve.py
# then open http://localhost:8000/web/index.html
```

`serve.py` is `python3 -m http.server` with caching turned off. That
matters more than it sounds: a cached `app.js` presents as a preset that
silently does nothing, and a cached `vxs.wasm` is worse — the page loads,
the Scheme runs, and a primitive added in the last build is simply absent.
Safari is the most aggressive about it, and its hard-reload chord is
Cmd-Option-R rather than the Cmd-Shift-R everything else uses.

### Watch mode

Editing Scheme in a `<textarea>` is no way to write real code, so the
page can instead watch files you edit in your own editor. Tick **Watch**
and give it a path (default `../demos/scratch.scm`): the page polls that
file and every `lib/*.scm` over the same static server, and re-runs on
save.

The part that matters is that the libraries are re-registered on each
pass. `lib/*.scm` is compiled into the wasm binary at build time, so a
library edit is normally invisible in the browser until `make` — watch
mode overrides the baked-in copies with the ones being served, and `load`
prefers the override. Editing `lib/points.scm` or `lib/wgsl.scm` takes
effect on save, with no rebuild.

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
