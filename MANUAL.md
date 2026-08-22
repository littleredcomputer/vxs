# The vxs Manual

Not a Scheme tutorial and not an API dump. This is the set of things that
are **specific to vxs**, that you cannot infer from the source, and that
you would otherwise learn by watching a fiber die.

Everything in here was verified against the engine rather than read off the
implementation. Where a rule has an edge, the edge is stated.

- [1. Fibers and futures](#1-fibers-and-futures)
- [2. Where you may suspend](#2-where-you-may-suspend) ← the one that bites
- [3. Errors: what is catchable](#3-errors-what-is-catchable)
- [4. The GPU pipeline](#4-the-gpu-pipeline)
- [5. Testing without a browser](#5-testing-without-a-browser)
- [6. Known gaps and open work](#6-known-gaps-and-open-work)

---

## 1. Fibers and futures

vxs has no threads and no re-entrant `call/cc`. Concurrency is cooperative
fibers, scheduled round-robin against a shared wall-clock budget.

```scheme
(future expr)     ; start a fiber; returns a future immediately
(touch fut)       ; block this fiber until fut has a value; return it
(yield)           ; give the scheduler a turn
```

A fiber runs until it yields, blocks, or finishes. Nothing preempts it
mid-expression, so **no fiber ever observes another's half-finished work** —
that is the whole contract, and it is why shared mutable state between
fibers is safe here in a way it is not with threads.

A fiber that overruns the frame budget without yielding is *preempted*, but
politely: it keeps an exclusive resume slot so it finishes its current
inter-yield section before any sibling runs. You will see

```
[vxs] 1 fiber(s) exceeded the frame budget without yielding
```

which is a warning about frame rate, not a correctness problem.

### Two kinds of future

The distinction matters for §2, and nothing in the syntax reveals it.

| kind | made by | settled by |
|---|---|---|
| **fiber-backed** | `(future …)` | the vxs scheduler |
| **host-settled** | `sleep`, `request-adapter`, `request-device`, `gpu-compile`, `gpu-buffer-read` | the JavaScript event loop |

A host-settled future is backed by a JS promise. Only the browser (or Node)
can complete it, and it can only do that when vxs **returns control to the
event loop**. That single fact generates every rule in the next section.

---

## 2. Where you may suspend

> **The rule.** `touch` of a host-settled future, and `yield`, must appear
> where the fiber can actually suspend: in ordinary Scheme code. They must
> not appear inside a form that is implemented as a native call.

Break it and the fiber dies with:

```
[VM Error] touch: cannot await a host-settled future here. This touch is
inside guard/map/apply/for-each/load, whose continuation includes native
frames, so the fiber cannot suspend and the event loop can never run to
settle it. Await it in the fiber body directly, outside that form.
```

### The table

Verified by probe, not by reading the source.

| you write | host-settled `touch` | `yield` |
|---|---|---|
| plain fiber body | ✅ | ✅ |
| `let`, `let*`, `letrec` body | ✅ | ✅ |
| `cond`, `if`, `begin` | ✅ | ✅ |
| named `let` loop | ✅ | ✅ |
| a `lambda` you call yourself | ✅ | ✅ |
| **`unwind-protect`** | ✅ | ✅ |
| `guard` | ❌ dies | ❌ dies |
| `dynamic-wind` | ❌ dies | ❌ dies |
| `map`, `for-each`, `vector-map` | ❌ dies | ❌ dies |
| `apply` | ❌ dies | ❌ dies |
| `load`, `force` | ❌ dies | ❌ dies |

`unwind-protect` being safe is not an accident and is worth knowing: it
compiles inline, because a pending cleanup is just a value on a list.
`guard` cannot, because it needs a real C++ `try`/`catch` scoped to a
specific instruction pointer, and C++ exception handling is tied to
call-stack depth. `dynamic-wind` is a subr, so it goes the same way.

### Why

`(guard (e …) body)` desugars to `(%guard (lambda (var) …) (lambda () body))`,
and `%guard` is a native subr. Between the scheduler and your `touch`:

```
run_dispatch(stop_at_depth = 0)          ← the scheduler's loop
 └ call_subr(%guard)
    └ subr_guard                          ← owns the C++ try/catch
       └ call_closure(body-thunk)
          └ run_dispatch(stop_at_depth = N)   ← RE-ENTERED
             └ your OP_TOUCH
```

To suspend, the fiber must return all the way out to the event loop.
Returning from the inner dispatch only lands in `call_closure`, then
`subr_guard` — C++ frames holding an active `try` block and saved
stack/frame/winder sizes, none of which can be captured into the fiber's
heap-allocated frames and restored later. **The continuation is partly in
the C++ stack**, and those frames are not first-class here.

### Fiber-backed futures are exempt

`touch` of a *fiber-backed* future works everywhere, including `guard` and
`map`:

```scheme
(future (guard (e (#t 'caught)) (touch (future 42))))   ; => 42
(future (car (map (lambda (x) (touch (future x))) '(1 2 3))))  ; => 1
```

Because that work lives inside the VM, the scheduler can be pumped in place
rather than suspended. Only the host event loop is out of reach.

So the rule is narrower than "no `touch` inside `guard`". It is: **no
`touch` of a host-settled future inside `guard`.**

### What to do instead

**Hoist the await.** Idiomatic vxs puts every host await in the `let*` at
the top of the fiber, then loops below it:

```scheme
(future
  (let* ((adapter (touch (request-adapter)))
         (device  (touch (request-device adapter)))
         (shader  (touch (gpu-compile device src))))
    (let loop ()
      (draw device shader)     ; no awaits in here
      (yield)
      (loop))))
```

**Or use `touch/or-error`**, which returns the error object instead of
raising, so it needs no handler, so it needs no native frame — and it still
compiles inline, so it can still suspend:

```scheme
(let ((r (touch/or-error (gpu-compile device src))))
  (if (error-object? r)
      (begin (display "shader rejected: ")
             (display (error-object-message r))
             (newline)
             #f)
      r))
```

⚠️ `touch/or-error` is an **alternative to** `guard`, not something that
works *inside* one. Putting it in a `guard` body dies exactly as `touch`
does — the native frames are the problem, and they are still there.

---

## 3. Errors: what is catchable

`guard` is R7RS and behaves normally for Scheme-level raises:

```scheme
(guard (e (#t (list (error-object-message e) (error-object-irritants e))))
  (error "boom" 1 2))
;; => ("boom" (1 2))

(guard (e ((symbol? e) 'by-clause)) (raise 'sym))   ; => by-clause
```

### JavaScript errors *are* catchable

A common misreading: that errors originating in JS cannot reach a `guard`
clause. They can, whenever the call is **synchronous**. Every GPU primitive
routes a JS `throw` back through `raise_contract`, and it arrives
indistinguishable from `(error …)`:

```scheme
(guard (e (#t (error-object-message e)))
  (gpu-wrangle! device buf shader n t seed))
;; => "gpu-wrangle!: device.createBindGroup is not a function"
```

The boundary is **synchronous vs. asynchronous**, not JS vs. Scheme. What
`guard` cannot contain is a suspension point (§2) — and note that a
*successful* host await fails there too, which is the giveaway that the
rule was never about exceptions.

### ⚠️ Native VM errors are NOT catchable

A real, unfixed gap. Contract violations raised by VM primitives blow
straight past `guard` to the top level:

```scheme
(guard (e (#t 'caught)) (car '()))
;; does NOT return 'caught — prints:
;; [VM Error] car: contract violation, expected pair, got ()
```

Same for `vector-ref` out of bounds and friends. If you need to survive
these, check before you call. `guard` is for `raise`, `error`, and
primitives that go through `raise_contract`.

### ⚠️ Arithmetic does not type-check

Non-numbers are silently treated as zero, and the result is promoted to a
flonum:

```scheme
(+ 1 'a)      ; => 1.0     (not an error)
(+ 'a 'b)     ; => 0.0
(* 2 'a)      ; => 0.0
(+ 1 "str")   ; => 1.0
(< 1 'a)      ; => #f
```

A deliberate consequence of keeping `+` first-class and un-opcoded for
speed, but it means a typo'd variable name in arithmetic produces a
plausible wrong number rather than a complaint. If a computation goes
quietly wrong, suspect this first.

---

## 4. The GPU pipeline

### Compile, then draw

Compilation is asynchronous and returns a future. The draw primitives take
the **handle** it settles with, never source — so nothing can reach a
pipeline without having waited for the compile to succeed.

```scheme
(let* ((adapter (touch (request-adapter)))
       (device  (touch (request-device adapter)))
       (shader  (touch (gpu-compile device wgsl-source))))
  (gpu-wrangle! device buf shader count t seed)
  (gpu-draw-buffer! device buf shader count t camera "gpu-canvas"))
```

A bad shader stops the program at the `touch`, before a single frame, with
the location and the offending line:

```
[vxs] fiber died: WGSL error at line 2:4 — no entry point found
    2 | fn BADSHADER() {
```

This matters more than it sounds. `createShaderModule` **does not throw**;
WebGPU reports compile problems asynchronously. Before this, a shader that
was not a WGSL program by any reading produced sixty frames a second of
nothing, every indicator healthy, and a black canvas — indistinguishable
from arithmetic gone NaN or geometry gone off-camera.

Handles are ordinary vxs handles: `(handle? h)`, `(handle-kind h)` →
`:gpu-shader`, `:gpu-adapter`, `:gpu-device`, `:gpu-buffer`.

### Buffers

Points are **seven floats, flat**: `x y z size r g b`.

```scheme
(make-points n)              ; a bytes object sized for n points
(points-view b)              ; an :f32 view over it
(point-set! v i x y z sz r g b)
(gpu-buffer device bytes)    ; upload; returns a buffer handle
(gpu-buffer-write! device buf bytes)   ; push host changes
```

`gpu-buffer` rounds the allocation up to a multiple of 16, so a buffer is
often slightly larger than the bytes you handed it.

### Reading back

```scheme
(gpu-buffer-read device buf)        ; whole buffer
(gpu-buffer-read device buf 84)     ; first 84 bytes
```

Settles with a **pair, `(frame . bytes)`** — not the bytes alone:

```scheme
(let* ((r     (touch (gpu-buffer-read device buf)))
       (frame (car r))
       (view  (bytes-view (cdr r) :f32)))
  (view-ref view 0))
```

⚠️ **The snapshot is lagged, unavoidably.** `mapAsync` settles a frame or
two after the copy is submitted, so what you get is the most recent
completed state and never the current one. Invisible while you are only
looking at it, and a correctness trap for anything that feeds the result
back in — hence the frame stamp, which turns "why is this unstable" into
"I am reacting to two-frame-old data".

Pass a length when you can. A full readback of sixty thousand seven-float
points is 1.7 MB a probe, and most questions want a summary a kernel could
have reduced to a few hundred numbers first.

### Which GPU calls are guardable

| call | shape | guardable |
|---|---|---|
| `request-adapter`, `request-device` | future | ❌ — hoist, or `touch/or-error` |
| `gpu-compile` | future | ❌ — same |
| `gpu-buffer-read` | future | ❌ — same |
| `gpu-buffer`, `gpu-buffer-write!` | synchronous | ✅ |
| all six draw primitives | synchronous | ✅ |

### Pausing

Every render loop keeps **drawing** while paused and stops only the
simulation clock. Freezing the scheduler instead would freeze the renderer
— it is a fiber like anything else — and the camera with it, which is
exactly when you most want to orbit.

---

## 5. Testing without a browser

`testcases/fake_webgpu.js` is a fake WebGPU good enough to exercise every
host path under Node. It computes nothing — no shader runs — and that is
the point: what it makes testable is everything *around* the shader, which
is where the silent failures live.

```js
const { installFakeWebGPU } = require('./testcases/fake_webgpu.js');
installFakeWebGPU({ compileMessages: (src) => [] });   // BEFORE requiring vxs.js
const createVxsModule = require('./web/vxs.js');
```

It must be installed **before** `web/vxs.js` is required: the handle table
is only built if none exists, and `navigator.gpu` is read at adapter
request.

Pumping needs the event loop to get a turn between steps, or no promise
ever settles:

```js
for (let i = 0; i < n; i++) {
  M.ccall('vxs_step_fibers', 'number', ['number'], [0]);
  await new Promise((r) => setTimeout(r, 1));
}
```

`make test` runs this as `test-gpu`, covering the host paths and all six
GPU presets. **Do not report a GPU change as working on the strength of
reading the diff** — that is what these are for.

---

## 6. Known gaps and open work

What is wrong, what is missing, and what was decided about each. Kept here
rather than in a separate file so that a rule and its known exceptions stay
next to each other.

Nothing here is scheduled. Items marked **agreed** have been explicitly
decided on; the rest are candidates.


### Correctness gaps

#### `guard` cannot catch native VM errors — **agreed, worth fixing**

Symptom in [§3](#3-errors-what-is-catchable). The cause is two error
mechanisms that grew apart, not a design decision:

| | behaviour | catchable | sites |
|---|---|---|---|
| `raise_contract` (`vx_vm.h:686`) | error object → `in_flight_raises` → `throw RaiseEscape` | ✅ | 8 |
| legacy | sets `current_fiber->state = Error`, writes `error_message`, returns unspecified | ❌ | 42 |

`subr_guard` catches `RaiseEscape`; the legacy path never throws, so `guard`
is never involved and the fiber is simply marked dead underneath it.

Evidence it is drift rather than intent: `raise_contract` is used only by
the most recently written primitives (bytes, views, GPU); the two print in
different formats (`[VM Error] car: …` vs `bytes-view: …`); and the 42
legacy sites are the same five-line block copy-pasted, which is a template,
not per-primitive judgement about what should be recoverable.

**The work.** Each site collapses to `vm.raise_contract("car: …")`. Blast
radius checked: no golden file and no test asserts on the `[VM Error]`
prefix, and an uncaught `RaiseEscape` already reaches `step_fiber` and
becomes `StepResult::Error`, so top-level reporting keeps working. One
judgement call — add the `[VM Error]` prefix at the *reporting* site rather
than in the message, so top-level output stays recognisable while
`error-object-message` hands `guard` clean text.

**Why the cost is acceptable (Colin).** Throwing is more expensive than
setting a field, but the exception path here is not performance sensitive,
and none of the exceptions we throw have a control-flow application the way
something like `StopIteration` would. One caveat, which sharpens the argument
rather than weakening it: `ContinuationEscape` (escape-only `call/cc`) *is*
a control-flow use of a C++ exception. But it is a single throw site, and
if a program ever put one in a hot loop the remedy would be to redesign
that program, not to make throwing cheaper — so the exception path stays
outside the performance envelope by construction. `RaiseEscape`, the one
this change would multiply, has no control-flow use at all.

#### Arithmetic does not type-check

Symptom in [§3](#3-errors-what-is-catchable): non-numbers are treated as
zero, so a typo'd name yields a plausible wrong number rather than a
complaint.

Entangled with keeping `+` first-class and un-opcoded for speed, which is a
deliberate trade — so this is **undecided**, not agreed. Any fix has to
answer what it costs on the benchmark suite before it is worth having.

---

### Planned

Items §1, §2, §8, §9 are done. Order below is the, revised after
seeing the compile-future work land — it is not the numbering order.

#### §7 — a live parameter block  ← **next**

Bigger than the `w.pad` it was first written as. Today a kernel constant is
baked into the shader SOURCE, so dragging a slider recompiles on every
change and the motion hitches. Making it a uniform is the difference
between "a knob you demonstrate and a knob you play."

`w.pad` is one spare float and would fit exactly one parameter, which is
why it was proposed — but the struct should grow to a few general-purpose
slots instead. It costs nothing, and per the report it is "the last thing
standing between a control panel and a real instrument."

Also in §7, unrelated and small: `seed` should be a `u32` rather than a
float, and `make` should degrade gracefully without emsdk.

#### §4 — `gpu-wrangle!` repeat count

Small, and it lifts steps-per-frame past the frame-budget warning. Run the
kernel N times under one encoder and one submit. Not fixable from Scheme:
looping there yields between dispatches and so costs a frame per step.

#### §5 — named scratch attributes

The genuine design item. A wrangle can only touch what the renderer already
reads; there is no way to carry a value the renderer ignores. Needed before
anything stateful — velocity, age, target — can live on the GPU without
being smuggled through a colour channel.

#### §6 — a third binding for read-only data

`@binding(2) var<storage, read>`, wanted when the read-only case starts. Also
addresses pipeline accumulation: constants baked into source mean changing
one yields a new source string. (Partly mitigated now that pipelines key on
the shader handle rather than source text, but the cause remains.)

#### §3 — `define-once`  ← last

Ranked last by the report: "the setter pattern turned out to be the
better shape anyway." Nearly free if picked up; `defined?` exists. The
report's "leaked fiber" half does **not** apply to `web/app.js`, which
clears fibers at line 179.

### Infrastructure

#### A staleness guard for `web/vxs.wasm`

The artifact is committed deliberately — clone-and-serve is the deployment
model, and `.git` is only 26 MB for 65 revisions of it, so size is not the
argument. The real risk is a committed `.wasm` drifting from the source it
claims to be, which is a silent wrong-version bug of exactly the class this
project keeps hunting.

Cheapest version: a test that fails if any source under `src/` is newer
than `web/vxs.wasm`.

#### Migrate the classic testcases into the ground-up suite

Colin's own idea, explicitly not urgent. The 13 `vx-test.scm` cases move
from I/O-diff-against-golden-file to `assert-equal`-on-return-value —
except `r4rstest.scm`, which is fundamentally a printing conformance suite
and has to stay on the I/O path. Keep `dynamic.scm` whatever happens: its
value is the stress profile (self-parsing 2300 lines), not the answer.

---

### Parked ideas

Not scheduled, kept so they are not rediscovered from scratch.

- **Per-cube orientation** in the cubes renderer.
- **Overcooked-shaped actors** — goal-directed agents for the outside demo,
  as opposed to the planning/goal work that stays inside the org.
- **`gpu.html`'s "Scheme it runs" pane** still shows the triangle program
  regardless of which demo is running.
- **`define-record-type`** — parked when WebGPU work took priority.
- **`OP_LOOP`** — named-let entry costs one closure plus one box per
  captured variable, per entry. The "thread, don't capture" idiom works
  around it; an opcode would remove it.
- **`amb`/Church via fibers** — reimplement probabilistic choice points on
  fibers instead of `call/cc`, once the VM foundation settles.

---

## Appendix: things that surprised someone once

- A shader compile error is a **fiber death**, not a catchable exception,
  unless you use `touch/or-error`. §2.
- `(handle-kind h)` returns a **keyword** (`:gpu-shader`), not a symbol.
- A whole-buffer readback returns more bytes than you uploaded, because
  `gpu-buffer` pads to 16. §4.
- `error-object-message` on a raise whose payload is a tag gives you the
  tag, not prose. Check `error-object?` before assuming there is text.
- Two wasm modules in one Node process interfere enough to distort timings.
  Benchmark one module per process.
- `guard` does not catch `(car '())`. §3.
