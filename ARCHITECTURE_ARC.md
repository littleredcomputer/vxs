# Vx-Scheme: Architectural Arc, Decision Record & Citations

This document preserves the complete architectural lineage, design decisions, research citations, and performance milestones of **Vx-Scheme** (2002–2026).

---

## 1. Lineage & Philosophy

Vx-Scheme began in 2002–2006 as a small, embeddable Scheme runtime tailored for real-time systems (such as Wind River VxWorks). Its defining characteristics were:
- **Cooperative Fiber Multitasking**: Instruction-quantized fibers yielding control without thread contention or OS locks.
- **Ahead-of-Time C++ Bytecode Compilation**: Compiling Scheme scripts directly into standalone native executables.
- **Real-Time Graphics & Simulation**: Driving visual simulations via lightweight coroutines.

---

## 2. Research Citations & Prior Art

### Lisp & Scheme to Shaders / WebGPU
1. **Varjo & CEPL** (Chris Bagley):
   - *URL*: `https://github.com/cbaggers/varjo`, `https://github.com/cbaggers/cepl`
   - *Significance*: Compiled a Common Lisp subset (*Vari*) directly into GLSL/SPIR-V with cross-stage macro expansion and live REPL shader reloading. Proved homoiconic S-expressions are an ideal meta-language for GPU compute and graphics pipelines.
2. **Aether** (Cornell University / Zhang et al.):
   - *URL*: `https://github.com/cornell-zhang/aether`
   - *Significance*: Embedded Domain-Specific Language for shaders with staged metaprogramming, preventing pipeline mismatches and automating uniform bindings.
3. **Church & Probabilistic Scheme** (Goodman, Mansinghka, Roy, Tenenbaum, 2008):
   - *URL*: `http://probmods.org/`
   - *Significance*: Seminal probabilistic programming language built as a Scheme dialect, executing stochastic trace-based MCMC and generative visual scene modeling.

### WebAssembly & In-Browser Scheme Engines
4. **Guile Hoot** (Spritely Institute):
   - *URL*: `https://spritely.institute/hoot/`
   - *Benchmark*: Compiles Scheme ahead-of-time to Wasm GC. For an interactive dynamic `eval` REPL, requires bundling compiler modules (~1.2 MB – 6.6 MB).
   - *Vx-Scheme Comparison*: Full in-browser live dynamic `eval` + single-pass compiler + register VM + fiber scheduler in a **~250 KB standalone binary** (1/20th the payload).
5. **BiwaScheme & Lips**:
   - *URL*: `https://www.biwascheme.org/`, `https://lips.js.org/`
   - *Significance*: JS-based interpreters; highlighted the need for zero-copy typed arrays and native WASM performance for 60 FPS graphics.

---

## 3. Major Architectural Milestones & Decision Record

### Milestone 1: 64-Bit NaN-Boxed Value Representation
- **Problem**: Legacy runtime used a 56-byte `std::variant` (`CellValue`) with heap allocation for integers, floats, and pairs, causing GC pauses and high memory footprint.
- **Decision**: Implemented strict 8-byte NaN-boxing (`vxs::Value` in `vx_value.h`).
  - Canonical Quiet NaN: `0x7FF8000000000000ULL`
  - High 16 bits encode type tags (`TAG_INT`, `TAG_NIL`, `TAG_TRUE`, `TAG_FALSE`, `TAG_SYMBOL`, `TAG_PTR`).
  - Doubles use unmasked IEEE-754 representation (zero overhead for math).
  - Bit 31 of `TAG_SYMBOL` (`0x80000000ULL`) distinguishes `:keywords` from symbols with zero heap allocation.
- **Outcome**: 7x reduction in value size, zero pointer dereferencing for scalar math and keywords.

### Milestone 2: Single-Pass Direct Bytecode Compiler
- **Problem**: Legacy bootstrapping required `vxs-bootstrap` compiling `compiler.scm` into a 68 KB C++ file full of static bytecode arrays.
- **Decision**: Implemented `Compiler` in `vx_compiler.h` as a single-pass C++ AST-to-bytecode translator with lexical upvalue resolution and fixed-slot stack frames.
- **Outcome**: Compilation in microseconds; zero bootstrap dependencies; 1.2-second full project build times.

### Milestone 3: Cooperative Fiber Scheduler for WebAssembly
- **Problem**: In-browser animation loops require non-blocking execution sliced at 60 FPS (16.6 ms per frame).
- **Decision**: Integrated quantum-stepped fibers (`vxsStepFibers(2500)` per `requestAnimationFrame`), with first-class `future` and `touch` synchronization primitives.
- **Outcome**: Smooth 60 FPS particle simulations, Henon/Clifford attractors, MCMC samplers, and 2D wave PDEs running in the browser canvas.

### Milestone 4: Modern Collections & Clojure-Inspired Ergonomics
- **Decisions**:
  - `[e1 e2 ...]` vector literals $\rightarrow$ $O(1)$ flat `ObjVector`.
  - `{:k1 v1 ...}` map literals $\rightarrow$ associative `ObjMap`.
  - Bracketed `let [x 10 y 20]` bindings across `let`, `let*`, `named let`, `do`.
  - Callable data structures: `(:key map)`, `(map :key)`, `(vec index)`.
  - Polymorphic `(get coll key/index [default])`.

### Milestone 5: Procedural `defmacro` & Auto-Gensym (`sym#`)
- **Decisions**:
  - Full procedural `defmacro` / `define-macro` evaluating Scheme AST transformers at compile-time.
  - Rich Hickey-style auto-gensym: any `sym#` inside quasiquotes `` `(...) `` automatically generates a unique identifier scoped to that expansion.
  - Variadic parameter lists `(lambda (a . rest) ...)` and unquote-splicing (`,@`).
  - Introspection via `(macroexpand form)`.

### Milestone 6: AOT Static Bytecode Standalone Native Compilation
- **Decision**: `vx-scheme --compile script.scm -o binary` compiles the Scheme AST ahead-of-time into raw static C++ hex bytecode arrays and builds a standalone native binary without linking the reader or compiler at runtime.

---

## 4. Current Repository Structure

```
vxs/
├── src/
│   ├── vx_value.h      # 64-bit NaN-boxed scalar value (8 bytes)
│   ├── vx_heap.h       # Compact heap objects (Cons, Vector, Map, String, Closure, Subr, Future)
│   ├── vx_reader.h     # S-expression reader with [...], {...}, :kw, quasiquote
│   ├── vx_compiler.h   # Single-pass bytecode compiler with defmacro & bracketed bindings
│   ├── vx_vm.h         # VM header, fiber scheduler, and symbol intern table
│   ├── vx_vm.cpp       # Bytecode dispatch, subr primitives, quantum stepping
│   ├── vx_wasm.cpp     # WebAssembly C-API exports & canvas telemetry
│   ├── main.cpp        # Native CLI runner, REPL, and AOT compiler
│   └── Makefile        # Lean build system for Native & WebAssembly
├── web/
│   ├── index.html      # Workbench UI with Monaco editor & live canvas
│   ├── app.js          # WebAssembly fiber driver & animation loop
│   ├── test_suite.js   # 58-test in-browser structured test harness
│   ├── vxs.js          # Emscripten JS glue
│   └── vxs.wasm        # 250 KB compiled WebAssembly binary
├── testcases/          # Benchmarks, test scripts, and node test runners
└── legacy/             # Archived 2002-2006 codebase (vx-scheme.h, cell.cpp, vm.cpp, etc.)
```
