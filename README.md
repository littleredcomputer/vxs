# Vx-Scheme

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
- **`call/cc` is currently disabled.** General re-entrant continuations
  were traded for a chunked, non-reallocating VM stack and the
  performance that comes with it — fibers + `yield` cover the common
  generator/coroutine use case. See `src/vx_vm.h` for the design
  rationale if you're curious.

## Project layout

```
src/            C++20 engine: NaN-boxed values, reader, compiler, VM, fiber scheduler
web/            Static in-browser workbench (Monaco editor, live canvas, demo presets)
testcases/      Ground-up test suite, r4rstest.scm, classic Scheme benchmarks
```

## License

TBD.
