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

### The current ports are fiber-local

`current-output-port` and `current-input-port` are **per fiber**, captured
at spawn from whatever the parent was using. A fiber that redirects its own
output does not affect anyone else, and one started inside a redirection
writes there:

```scheme
(with-output-to-string
  (lambda ()
    (display "parent:")
    (let ((f (future (display "child")))) (run-fibers) (touch f))))
;; => "parent:child"
```

This has to be fiber state rather than one VM slot, and the reason is worth
knowing because it is not about fibers dying. `with-output-to-string` saves
the current port, rebinds, and restores under `unwind-protect`. If another
fiber rebinds between the save and the restore, it captures the *first*
one's binding as its "previous":

```
A: prev := stdout,  set A-port,  yields
B: prev := A-port,  set B-port,  yields      ← captured A's binding
A: writes to B-port, restores stdout
B: writes to stdout, restores A-port         ← global left on a dead port
```

Every cleanup ran, in the right order, and the result was still wrong —
after which every `display` in the VM went into a string nobody reads.
`unwind-protect` restores what it saved; what it saved was not that
fiber's to own. Making the binding fiber-local is what makes the restore
meaningful, and it also means an abandoned fiber's redirection dies with
it rather than needing a cleanup to run at all.

### `in-generator?`

```scheme
(in-generator?)   ; #t only when the current fiber is driven by `resume`
```

`(yield)` outside a generator is **legal** — every demo yields to the
scheduler once a frame — so `yield` cannot complain on its own behalf. A
form that expects an *answer* back has to ask, and this is the question.

Without it a function full of yields, called directly, runs on: it
yields, and the scheduler resumes it with unspecified. Arithmetic now
refuses a non-number (§3), but that backstop is downstream and fires only
if the unspecified reaches arithmetic at all — this predicate asks the
structural question at the moment of the mistake, and names it.

```scheme
(define (at address distro)
  (if (not (in-generator?))
      (error 'at "not inside a generative function"))
  (yield (cons address distro)))
```

A plain `future` is **not** a generator, even though `yield` works in
both — that is the distinction the predicate exists to draw.

### Generators — a fiber driven by hand

```scheme
(generator proc arg ...)   ; the arguments are passed to proc
```

The arguments go **here** rather than being closed over, and the reason is
§2's table. `(generator (lambda () (apply proc args)))` fails — `apply`
calls a closure from native code, so a `yield` inside `proc` crosses a
native frame and dies with *"yielded mid-call"*. Closing over **literal**
arguments is fine; a procedure whose arguments arrive as a **list** has no
way to spread them without `apply`.

```scheme
(apply generator proc args)    ; ✅  apply on the SUBR is fine
(generator (lambda () (apply proc args)))   ; ❌ apply on a yielding closure
```

Variadic procedures get their rest list. Arity is checked.

The other thing a fiber can be, and not a future with an extra argument.

| | future | generator |
|---|---|---|
| who resumes it | the scheduler, whenever | exactly one caller, deliberately |
| how many results | **one**, memoised forever | a different one each time |
| runs on its own | yes, round-robin | **no** — only when resumed |

```scheme
(define g (generator (lambda ()
                       (let loop ((got (yield 'ready)))
                         (loop (yield (list 'saw got)))))))

(resume g)          ; => ready
(resume g 'apple)   ; => (saw apple)
```

`(yield v)` is an **expression**: its value is whatever the resumer passed
to `(resume g v)`, or unspecified when the ordinary scheduler resumed it.
That is what makes the two halves able to talk rather than just take turns.

The last `resume` returns the thunk's **return** value, not a yielded one.
`(generator-live? g)` afterwards is what tells them apart — asked after,
not before, since the call that finishes it is the one that returns.

A generator is not in the scheduler's ring, so it makes no progress
between resumes, and it owns its fiber outright. Abandoning one mid-run
collects it with its fiber — and, per the same rule `vxs-clear-fibers`
follows, **without running its pending `unwind-protect` cleanups**.

Abandoning them is safe. A `SlabStack` allocates 32 KB the moment it
exists, so `make_generator` charges the collector for that — otherwise the
GC counts a forty-byte object and lets an unbounded amount of fiber pile
up behind it. Measured over 20,000 abandoned mid-run: **222 MB peak RSS
across four collections before that charge, 2.4 MB after.**

⚠️ A **future** is different, and dropping the reference does *not* make it
garbage: its fiber stays in the scheduler's ring until it completes, so it
is rooted whether you hold it or not. 20,000 concurrent futures really do
cost ~640 MB of stacks — that is honest live memory, not a leak, and the
fix is to spawn fewer at once, not to collect harder.

Two things it refuses rather than hangs on: resuming a generator that is
already running (directly or round a chain), and `touch`ing a future from
inside one — nothing will ever settle it, because nothing but `resume`
will ever run that fiber.

`testcases/amb.scm` builds backtracking search on this: a fiber suspends
but does not **rewind**, so search re-executes from the top with a
different answer from an oracle at each choice point, and yields each
solution as it is found.

---

## 2. Where you may suspend

> **The rule.** `touch` of a host-settled future, and `yield`, must appear
> where the fiber can actually suspend: in ordinary Scheme code. They must
> not appear inside a form that is implemented as a native call.

Break it and the fiber dies with:

```
[VM Error] touch: cannot await a host-settled future here. This touch is
inside map/apply/for-each/load/force or dynamic-wind, which call your code
from native frames, so the fiber cannot suspend and the event loop can
never run to settle it. Await it in the fiber body directly, outside that
form. (guard is fine — it compiles inline.)
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
| **`guard`** | ✅ | ✅ |
| `dynamic-wind` | ❌ dies | ❌ dies |
| `map`, `for-each`, `vector-map` | ❌ dies | ❌ dies |
| `apply` | ❌ dies | ❌ dies |
| `load`, `force` | ❌ dies | ❌ dies |

`unwind-protect` and `guard` are safe for the same reason: both compile
**inline**, so their bodies run in the fiber's own dispatch loop with no
C++ frames above them. A pending cleanup is a value on `Fiber::winders`; a
live handler is a record on `Fiber::handlers`. Both are fiber state, so
both survive a suspension.

`guard` was in the ❌ column until it was compiled inline. It used to
desugar to a subr that ran its body through `call_closure` inside a C++
`try` — and `try`/`catch` really is tied to C++ call-stack depth, which is
what made that look unavoidable. The way out was to stop needing one per
guard: `raise` searches `Fiber::handlers` instead, and there is a single
`catch` per **dispatch loop** rather than one per guarded call.

The rest of the ❌ row is unchanged and is not the same problem. `map`,
`apply`, `for-each`, `load` and `force` genuinely do call closures from
native code; there is no body to compile inline. `dynamic-wind` is still a
subr.

### Why

Anything in the ❌ column calls a closure from native code:

```
run_dispatch(stop_at_depth = 0)          ← the scheduler's loop
 └ call_subr(map)
    └ call_closure(your lambda)
       └ run_dispatch(stop_at_depth = N)   ← RE-ENTERED
          └ your OP_TOUCH
```

To suspend, the fiber must return all the way out to the event loop.
Returning from the inner dispatch only lands in `call_closure`, then
`map` — C++ frames holding live local state that cannot be captured into
the fiber's heap-allocated frames and restored later. **The continuation is
partly in the C++ stack**, and those frames are not first-class here.

`guard` used to appear in exactly this diagram, with `subr_guard` and its
C++ `try` in place of `map`. The fix was not to make C++ frames
suspendable — it was to stop creating them. `guard` compiles inline, and
`raise` finds its handler by searching `Fiber::handlers`, with one `catch`
per dispatch loop instead of one per guarded call. `map` has no such
route: there is no body to compile inline, only a closure it must call.

### Fiber-backed futures are exempt

`touch` of a *fiber-backed* future works everywhere, including `guard` and
`map`:

```scheme
(future (guard (e (#t 'caught)) (touch (future 42))))   ; => 42
(future (car (map (lambda (x) (touch (future x))) '(1 2 3))))  ; => 1
```

Because that work lives inside the VM, the scheduler can be pumped in place
rather than suspended. Only the host event loop is out of reach.

So the rule is narrower than it looks: **no `touch` of a host-settled
future inside `map`, `apply`, `for-each`, `load`, `force`, or
`dynamic-wind`.** `guard` is no longer on that list.

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
raising. Less essential than it was — `guard` can hold a host await now —
but still the tidier shape when a failure is a value you want to branch on
rather than an exception you want to escape with:

```scheme
(let ((r (touch/or-error (gpu-compile device src))))
  (if (error-object? r)
      (begin (display "shader rejected: ")
             (display (error-object-message r))
             (newline)
             #f)
      r))
```

`touch/or-error` works inside a `guard` now, as does `touch` — both
compile inline, and so does `guard`. Neither works inside `map`.

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

### What `guard` does not see

`guard` catches **raised conditions** — `raise`, `error`, and anything a
library signals through them. It does not catch **contract violations**
from VM primitives:

```scheme
(guard (e (#t 'caught)) (car '()))
;; does NOT return 'caught — prints:
;; [VM Error] car: contract violation, expected pair, got ()
```

Same for `vector-ref` out of bounds and friends. This is a **boundary, not
a gap**, and the line it draws is the one R6RS draws between `&violation`
(the program broke a rule — a bug) and `&error` (the world misbehaved).
R7RS-small draws it too, in what it chose to omit: the only error
predicates it standardizes are `file-error?` and `read-error?`, both
environmental. Neither standard offers a way to ask "was that a bad
argument to `car`?", because the answer does not help you.

Mechanically the difference is that a contract violation never raises at
all. It sets the fiber's error state and returns a sentinel; the dispatch
loop notices and stops the fiber. There is no condition to catch.

#### Crossing the boundary deliberately

A dead fiber is still **observable from outside**. Whoever touches its
future gets the failure, and there `guard` works normally:

```scheme
(guard (e (#t (report e)))
  (touch (future (eval user-form))))
```

That catches everything, contract violations included, at the cost of one
fiber. It is the right shape for an `eval` boundary — a REPL, a livecoding
page loading a file someone just typed — where the whole point is that a
bug in the input must not take down the host. Use `touch/or-error` for the
same thing without a handler.

`unwind-protect` cleanups still run when a fiber dies this way, so
resources are released on both paths.

⚠️ What you get is an error object carrying a **string**. You can report
it; you cannot dispatch on what kind of fault it was. Giving faults
structure — R6RS's `&who`/`&message`/`&irritants` — would improve this
supervision path without making them catchable in place. The two are
independent, and only the first is worth wanting.

### Arithmetic refuses non-numbers

A non-number reaching a numeric primitive is a contract violation:

```scheme
(+ 1 'a)      ; [VM Error] +: contract violation, expected a number, got a
(< 1 'a)      ; same — no plausible #f from comparing against a phantom 0.0
(max 'a -5)   ; same — max used to RETURN the symbol
(remainder 5 0) ; division by zero, same mechanism. (/ x 0) stays inf: IEEE.
```

It was not always so: `as_real()`'s fall-through used to turn anything
that was not a number into 0.0, so a typo'd variable gave a plausible
number — and a plausible *boolean* from the comparisons, which silently
picks a branch. The old behaviour was recorded as a speed trade, but the
trade was illusory: the tag tests were already executed on every call and
their answer discarded, so the check changed only what the fall-through
arm does (measured: within noise on a tight arithmetic loop).

Like every contract violation, this is a native VM error — uncatchable in
place by `guard`, observed across a fiber boundary via `touch/or-error`
(section 3). Inside a generator, `resume` re-raises it in the driver's
context, so a typo'd model stops an `importance` run with the operation
and the offending value named rather than converging on nonsense.

---

## 4. The GPU pipeline

Four plumbing sections first — how a shader is compiled, how buffers move,
how to get a number back, and what may be wrapped in a `guard`. Then the
three kinds of data a kernel can see, then the kernel language itself, then
how a dispatch is scheduled.

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

### Live parameters

A constant written into a kernel lives in the shader *source*, so changing
it recompiles — dragging a slider hitches on every frame it moves. The
wrangle uniform has eight spare slots for values that change per frame:

```scheme
(wrangle-params! '(sigma radius gain))   ; bare symbol means :f32
(wrangle-params! '((sigma :f32) (mode :u32) (flatten :flag)))

(define PB (make-wrangle-params))        ; made ONCE
(define PV (wrangle-params-view PB))
(param-set! PV 'sigma 0.7)               ; no allocation
(param-set! PV 'flatten #t)
(gpu-wrangle! device buf shader n t seed PB)
```

Parameters are **typed**, and the type decides which slot they occupy:

| type | slots | in a kernel |
|---|---|---|
| `:f32` | 16 (`p0`…`p15`) | `sigma` → `w.p0` |
| `:u32` | 8 (`i0`…`i7`) | `mode` → `w.i0` |
| `:flag` | 32 bits of `flags` | `flatten` → a `:bool`, so `(if flatten a b)` works |

Flags also get an emitted `fn flag_flatten() -> bool` for WGSL-text
bodies, the same dual treatment attributes get.

The struct's members occupy 116 bytes, which WGSL rounds to 128 since a
uniform struct's size is a multiple of 16. Substeps are addressed by
dynamic offset and `minUniformBufferOffsetAlignment` is 256, so each
substep's slice costs 256 bytes whatever the struct holds — which is why
widening it was free. **256 is the real wall**: past it the allocation and
the per-substep write both double, and the answer there is a read-only
struct passed down as a parameter, not a wider uniform.

⚠️ `:u32` and `:flag` are not tidiness. An f32 carries 24 mantissa bits, so
an integer or a bitfield in a float slot works perfectly up to bit 23 and
then **silently drops the rest** — a failure curve that survives every test
written early. Carry counts, indices and bitfields as integers.

This is also what removes the recompile-to-toggle pattern: a mode baked
into shader source means changing it rebuilds the shader, and a mode in a
flag bit costs nothing.

One declaration is the single source of truth for both sides, so a typo is
an error rather than a silently wrong slot. The block is a bytes object
written in place rather than a fresh list per frame, because at sixty
frames a second a list allocates sixty times a second and these demos
otherwise run at zero objects per frame.

`run-wrangle-loop` takes it as an optional argument after the canvas id.
Omit it and the slots read zero.

⚠️ A declared parameter or attribute may not take a built-in's name
(`time`, `count`, `seed`, `step`). Declarations shadow built-ins in the
kernel environment, so a parameter called `seed` would quietly resolve to a
parameter slot instead of the uniform's seed. Refused rather than
documented.

### Scratch attributes

State the simulation needs and the renderer must not see — weight, age,
velocity, an index into another element. Declared once; the library
computes offsets
and generates accessors for both sides:

```scheme
(scratch-attributes! '((charge :f32) (age :f32) (source :u32)))

(define SB (make-scratch n))       ; a second buffer, bound at 2
(define SV (scratch-view SB))
(scratch-set! SV 0 'charge 0.5)
(scratch-ref  SV 0 'charge)

(run-wrangle-loop seed n src frame! cam :canvas "gpu-canvas" :scratch SB)
```

In a kernel, an attribute name means **this point's** value, the way VEX
means `@Cd` — every invocation owns exactly one index:

```wgsl
attr_charge_set(i, charge * 0.99);      // read `charge`, write via the setter
```

Types are `:f32`, `:u32` and `:vec3f`. `:u32` is not decoration — an index
stored as a float aliases past 2²⁴, the same failure the seed had. Integer attributes are `bitcast` in the shader and read through a
second view on the host, so the round trip is exact in both directions.

A `:vec3f` is three flat floats with the accessor building the vector,
because `vec3<f32>` carries 16-byte alignment inside a storage array.

Declaring nothing emits nothing: a kernel with no attributes compiles to
exactly the text and the two-binding layout it always did.

⚠️ Both `scratch-attributes!` and `wrangle-params!` **replace** rather than
extend, and the kernel environment is rebuilt from both. They do not clear
each other.

### Pose

Orientation is a **stock attribute**. Declare one and the cube renderer
turns:

```scheme
(scratch-attributes! '((pose :quat)))       ; the convention: name and type
```

```wgsl
attr_pose_set(i, q_from_rotvec(f * twist));  // in the kernel, once per element
```

`:quat` is four floats, **(x y z w) with the scalar last**, matching the
posquat order `px py pz qx qy qz qw`. Position stays in the point buffer
and orientation in the scratch buffer — the same information, split, which
also means the sprite renderer never strides past four floats it does not
read.

An attribute **named `pose`, of type `:quat`** is what the cube shader
looks for. Declaring nothing leaves the shader byte-for-byte unchanged, and
the accessor's stride and offset come from the same declaration the kernel
compiles against, so the two cannot disagree about where a pose lives.

**One buffer, two pipelines** — read-write at binding 2 for the compute
pass, read-only at binding 2 for the draw. No second copy, no upload, no
synchronisation to get wrong.

⚠️ **Generate from a rotation vector, store a quaternion.** The two forms
are good at different jobs:

- A **rotation vector** (`axis · angle`) is what a field hands you, and it
  is continuous everywhere *including* zero — precisely where the axis
  stops meaning anything, the angle vanishes. Aiming an axis at a direction
  cannot manage that: there is no continuous way to choose the remaining
  roll, so it snaps somewhere, and on a slow field the snap is what the eye
  finds.
- A **quaternion** is what to store, because the renderer applies the
  rotation **36 times per cube** — once per vertex, and nothing amortises
  across them. `q_rot` is two cross products and *no* transcendental.
  Storing the rotation vector instead would put a `sin`, a `cos` and a
  `normalize` in all 36.

So `q_from_rotvec` runs once per element in the kernel, and `q_rot` runs
per vertex. One trig call instead of thirty-six.

Against a `mat4x4`: sixteen floats to four, read per-vertex where bandwidth
is the cost, and a matrix admits shear and scale nothing here wants. It is
marginally cheaper to apply — about 15 flops to 20 — and that is the least
important number in the comparison.

`lib/quat.wgsl` also has `q_mul`, `q_conj`, `q_from_axis_angle`,
`q_identity` and `q_normalize`. Normalising only matters if a program
*integrates* orientation over time; a quaternion recomputed from a field
each frame cannot drift.

### Shared read-only data

Data every element **reads**, as against scratch, which each element
**owns** — a lookup table every element consults, a per-frame input every
element reads. Scratch is addressed `scratch[i * stride + off]`, so it is
per-element by construction; the parameter block is a fixed handful of
scalars; and
anything that changes per frame cannot be baked into the source.

```scheme
(shared-layout! '((table 48) (samples 41)))   ; named regions, in order

(define SH (make-shared))
(define SHV (shared-view SH))
(shared-set! SHV 'samples 0 2.5)
(run-wrangle-loop … :shared SH)            ; re-uploaded every frame
```

Each region gets an accessor carrying its offset, so a kernel calls
`(shared-samples k)` and never writes an offset by hand. Indexing past a
region's end raises rather than quietly reading its neighbour.

Bound at 3 as `read-only-storage`. ⚠️ The WGSL identifier is **`sdata`**,
not `shared` — `shared` is a reserved word in WGSL.

Unlike `:scratch`, which uploads once, `:shared` is re-uploaded before
every dispatch: the case it exists for is data that changes each frame.

### `modulo` and `remainder`, not `mod`

Both exist and they are different operations:

| form | rounds toward | sign follows | same as |
|---|---|---|---|
| `(remainder a b)`, `(% a b)` | zero | the **dividend** | WGSL `%`, C `fmod` |
| `(modulo a b)` | −∞ | the **divisor** | GLSL `mod`, Scheme `modulo` |

They agree whenever both operands are non-negative and disagree everywhere
else — which is why neither is called `mod`. A reader arriving from GLSL
and a reader arriving from WGSL would read that name as opposite things,
and the disagreement only shows up once something crosses zero, which on a
centred grid or a noise field is constantly.

`%` is a synonym for `remainder`, and is safe where `mod` was not: the
glyph is WGSL's own spelling, so it can only mean what WGSL means by it.
The ambiguity was in the word, not the operation.

⚠️ The two diverge on a negative **dividend**, not a negative divisor.
`(modulo -1 3)` is `2` and `(remainder -1 3)` is `-1`, both with a
positive divisor. Wrapping a coordinate that has gone negative back into
`[0, b)` is the case that cares, and it is the ordinary one — negative
divisors are the rare thing, and not where the hazard lives.

On `:u32` operands `modulo` emits a plain `%`, since with nothing negative
the two coincide and the floor would be dead work.

`(modulo a b)` on floats expands to `a - b * floor(a / b)`, which names
both operands twice — so both are bound to locals first. That matters
because compiled results are spliced as **text**: naming an operand twice
would evaluate it twice, and `(modulo (random-uniform 0 1) k)` would
otherwise draw two different numbers and combine them.

### n-ary forms follow Scheme

Where Scheme defines an n-ary meaning, the kernel language compiles it
faithfully — because a `define-dual` body must mean the same thing in
both languages:

| form | compiles to |
|---|---|
| `(and a b c)`, `(or …)` | `((a && b) && c)`, folded left |
| `(min a b c)`, `(max …)` | `min(min(a, b), c)`, folded left |
| `(< lo x hi)` (all comparisons) | `(lo < x) && (x < hi)` — the chain |
| `(/ a)` | `(1.0 / a)` — the reciprocal |

A chained comparison's **middle** operand appears in two comparisons, so
it is bound to a local first — splicing its text twice would evaluate it
twice, and a draw must not run twice (the property `modulo` already
protects).

Everything else refuses a wrong argument count with the form named.
These used to be read **blindly**: `(sin time time)` dropped its second
operand and `(< 0.0 t 1.0)` compiled to `(0.0 < t)` — accepted,
plausible, and missing its upper bound.

### `if` is a selection, not a branch

In the kernel language `(if c a b)` compiles to WGSL `select(b, a, c)`.
Both arms are evaluated; the condition chooses which value is kept. This is
a **guarantee**, not an implementation detail:

> Every `random-*` call site in either arm executes exactly once per
> evaluation of the enclosing form. The number of draws a kernel consumes
> is a static property of its text, independent of any data.

That last sentence is the reason to want it. Stream alignment across points
and substeps is guaranteed rather than hoped for, and a paused frame is
reproducible by construction. It also matches the hardware: a divergent
branch on a GPU executes both paths anyway, so a "real branch" would be a
lie about cost dressed as a saving.

⚠️ The consequence to keep in mind: a conditional draw is not conditional.

```scheme
(if lost? (random-normal 0.0 1.0) 0.0)   ; the draw happens either way
```

The value is discarded when `lost?` is false, but the RNG stream advances
regardless. If you want the other thing — data-dependent consumption — it
needs a construct that deliberately does not look like `if`, and none
exists yet.

`if` also hoists **both** arms' `let`-lifted statements, not just the tail
expression.

### A wrangle in Scheme

`wrangle-wgsl` takes WGSL text and remains the escape hatch. `wrangle-scheme`
takes an expression and compiles it:

```scheme
(wrangle-scheme
  `(let* ((q   (+ (* position scale) (vec3 (* time drift) 0.0 0.0)))
          (f   (perlin3v q field-seed))
          (mag (length f)))
     (point position (* gain mag) (heat-colour mag)
            (pose (q-from-rotvec (* f twist))))))
```

A body sees `position`/`P`, `pscale`, `colour`/`color`/`Cd`, `index`, every
declared parameter and attribute, and everything in the kernel language. It
must end in `point`, whose type no operator accepts — which is what confines
it to terminal position rather than a rule to remember.

**Fields are total, attributes are partial**, and the asymmetry is legible
from the storage layout: `pt_write` is one packed write of seven floats, so
a partial point would have to read back what it did not mention, while
attribute setters are independent and omitting one emits nothing. Name the
input to say "unchanged" — `(point position pscale colour)`.

Attribute names are checked against `scratch-attributes!`, so a misspelling
fails at expand time with the name in hand rather than at shader compile as
an unresolved call.

⚠️ A knob may share a name with a builtin function without shadowing it —
operator position and variable position are separate. A parameter called
`floor` resolves to its slot, and `(floor x)` still calls the function.

### Bounded folds

The kernel language is pure-expression, and `fold-i` keeps it that way: an
accumulator and a **compile-time** bound, no mutation, no break, no early
exit. The whole form is one value, so it nests inside arithmetic and inside
itself.

```scheme
;; Sum over 41 directions; for each, take the nearest of 12 segments.
(fold-i 41 0.0 (k acc)
  (let* ((ang (+ theta (* (f32 k) 0.0785)))
         (dir (vec2 (cos ang) (sin ang)))
         (d   (fold-i 12 far (w best)
                (min best (seg-hit o dir (seg-a w) (seg-b w) far)))))
    (+ acc (score (shared-samples k) d noise))))
```

The index is **`:u32`** — an address, not a quantity. WGSL has no implicit
coercion, so using it as a number says `(f32 k)`. The body must have the
accumulator's type, and the bound must be a literal: a runtime bound is a
different performance object on a GPU, and a static one is what lets the
draw count stay a property of the text.

The body's own `let` bindings are emitted **inside** the loop. Every other
form here hoists its statements; hoisting these would evaluate them once
against the first index and reuse the answer for every iteration.

⚠️ `let` binds **sequentially** here — it lowers to a run of WGSL `let`
statements, so each binding is in scope for the next. `let*` is accepted as
the same form. This differs from R7RS `let`, which is parallel.

### Gradient noise

`lib/noise.wgsl` provides `perlin3`, `perlin3v` (three fields as a vector)
and `fbm3` (octaves).

```scheme
(perlin3v (* position scale) field-seed)    ; -> :vec3f
```

The seed is a `:u32` and a Threefry **key**, so neighbouring seeds give
unrelated fields — which is how `perlin3v` builds a vector field out of
three scalar ones.

Perlin needs a pseudo-random gradient at every integer lattice point, and
the usual route is a permutation table or a hand-rolled integer hash — both
invented, neither checkable, and a poor one shows as visible lattice
structure. A counter-based RNG is addressed **by index**, and a lattice
point *is* an index, so the gradient is one Threefry block with the
coordinates as its counter. No table, and the generator underneath is the
one already checked against published vectors.

**Range.** `perlin3` returns roughly **[−0.6, 0.6]**, not [−1, 1] —
measured as [−0.59, 0.62] with mean 0.001 over 64k samples. Mapping it as
though it were unit-range wastes about 40% of a colour or size budget.
It is **exactly zero at every integer lattice point**, which is the
defining property of gradient noise and a useful thing to test against.

**Sampling rate is the parameter that matters.** Perlin varies over one
lattice cell, so what a picture looks like depends on how many samples fall
inside a cell. Sampling at 3–4 per cell gives visible speckle and moiré —
honest structure, but aliased. Around 8–10 per cell reads as flowing
regions. For a grid of `n` elements spanning `w` world units at a given
`scale`, that ratio is `n / (w * scale)`.

⚠️ `wgsl-declare!` **asserts** a signature for hand-written WGSL; it does
not check that the WGSL exists. Declare a function whose source is not in
the assembled shader and the kernel language will type-check calls to it
happily, then fail in the browser with `unresolved call target`. Layer 18
now asserts that every declared name has a matching `fn` in the assembled
source, which is the only place that can be checked without a GPU.

⚠️ It deliberately does **not** go through `rng_init`. Those helpers keep
per-invocation state in `var<private>`, so noise routed through them would
silently consume a kernel's draws and shift every random decision after it.

### Substeps

Run the kernel N times per frame, inside one encoder and one submit:

```scheme
(gpu-wrangle! device buf shader n t seed params 8)
(run-wrangle-loop seed-bytes n src frame! camera "gpu-canvas" params 8)
```

`:draw` picks the renderer — `:points` (default) draws each element as a
sprite, `:cubes` as solid geometry. The buffer is the same seven floats
either way, so it is a choice at the call site rather than a different
program.

This cannot be done from Scheme. A loop there has to `yield` between
dispatches, so N steps cost N *frames* rather than one — the difference
between a simulation that outruns the frame budget and one pinned to it.

Each substep gets its own slice of the uniform and its own value of
`step`, which the preamble hands to `rng_init` as the stream index. That
matters more than it sounds: with a single stream, N substeps replay the
*identical* random draws N times. Positions still move, because each step
reads what the last one wrote, so it looks like it is working — but every
random decision repeats. `step` is also readable from a kernel.

WebGPU tracks the read-write hazard on the storage buffer itself, so
dispatch k+1 sees what dispatch k wrote with no explicit barrier; there are
no manual barriers in the API at all.

⚠️ The float slots are `p0 : f32, p1 : f32, …` in the struct rather than
`array<f32, 8>` on purpose: in the uniform address space
an array's stride is padded to 16 bytes, so the array spelling would cost
128 bytes and index wrongly for anyone assuming the floats were packed.

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

### Maps are ordered hash maps

A `{…}` map keeps its keys in **insertion order**, and `map-set!` on a
key that is already there replaces the value in place, without moving
it. Printing, `map-keys` and `map-values` all follow that order. Lookup
is **O(1)**: an index beside the ordered entries finds a key's position.
This is how JavaScript's `Map`, V8's ordered hash tables and CPython's
`dict` are built.

The order is load-bearing, not a nicety. `lib/wgsl.scm` emits its
definitions in insertion order because WGSL has no forward declarations.
More broadly, an unordered map iterates in hash order, and here that means
**address** order, which changes between runs and between the native and
wasm builds. Printed maps and emitted shader text would stop being
reproducible.

Keys are compared by **identity** (`eq?`), so symbols, keywords and
numbers work as keys and a freshly built string does not find an equal
one. Identity is also what makes every key hashable: its raw bits are the
hash. That is sound because the collector never moves objects.

A map under **64 keys carries no index** and scans instead. Measured, a
scan at that size is lost in the interpreter's own per-call cost, and
most maps are records of a handful of keys. At 64 keys a map builds its
index and keeps it, even if it later shrinks:

| keys | 16 | 256 | 4096 | 65536 |
|---|---|---|---|---|
| before the index, ns/lookup | 95 | 133 | 580 | — |
| now | 97 | 95 | 98 | 106 |

`map-delete!` is O(n): it preserves order by closing the gap, then
rebuilds the index. That's fine while deletion is rare.

`{:a 1 :b}` is a read error: the last key has no value. `{:a 1 :a 2}` is
**one** entry, `{:a 2}`, which is JavaScript `Map`'s rule. Both used to
be accepted wrongly: the first as `{:a 1}`, the second as two entries
whose lookup found the first.

### Maps: a missing key is `#f`

```scheme
(map-ref m :absent)          ; => #f
(:absent m)                  ; => #f      keyword-as-procedure
(map-ref m :absent 'DEFAULT) ; => DEFAULT
(:absent m 'DEFAULT)         ; => DEFAULT
```

`#f` rather than `'()`, because only `#f` is false in Scheme — `'()` is
**true**, so `(if (map-ref m k) …)` took the *present* branch for an absent
key. The keyword shorthand is borrowed from Clojure, where `(:y m)` is
`nil` and nil is falsy; the spelling came across without the truthiness
that made it safe.

⚠️ This does **not** disambiguate. A key whose stored value *is* `#f` is
indistinguishable from an absent one, and nothing fixes that but
`map-has?`. Use the bare two-argument form only where you know the key is
present or `#f` isn't a possible value; otherwise pass a default you chose,
or ask `map-has?`.

---

### Maps: copy and delete

```scheme
(map-copy m)        ; shallow copy — a fresh spine, shared values
(map-delete! m k)    ; remove k if present; a no-op otherwise
```

Scheme's convention is that aggregates are mutable and you copy
explicitly — `vector-copy`, `string-copy`, `list-copy`. Maps had no
`-copy`, so handing a map out of a structure handed out that structure's
own storage, and editing what looked like a candidate silently edited the
original.

⚠️ **Shallow**, like every other `-copy`. A nested map is *shared*, so a
structure of maps is protected only one level deep and the sharing is
invisible until something writes through it. Copy per level if you need
independence all the way down.

---

### `define-record-type`

R7RS-small, in the prelude. Constructor, predicate, accessors, optional
modifiers:

```scheme
(define-record-type <point>
  (make-point x y) point?
  (x point-x)
  (y point-y set-point-y!))
```

A **distinct heap type**, `ObjRecord`. The tag is a fresh object per
definition, so identity is by `eq?` and two record types sharing a name
stay distinct. Fields are fixed indices, so an accessor is one indexed
read rather than the linear scan a map lookup would be. Records print as
`#<point 3 4>`.

What it buys over a map or a closure is **nominal identity**. A map is
anonymous — there is no `point?` to write. A closure carries behaviour but
cannot be asked what it is without *calling* it, and calling an arbitrary
object to discover its type is not a predicate: it has side effects, it
errors on non-procedures, and it can hang.

The two compose rather than compete: a record whose field *is* a dispatch
closure gives you both — `predicate?` for identity, the closure for
behaviour, open to extension without touching the record.

⚠️ The constructor spec is a **proper list of declared field names**, as
R7RS says. A rest argument (`(make-g f . rest)`) or a name that isn't a
declared field is refused at expansion time — both used to be accepted and
then quietly misbehave, the rest arguments going nowhere and the
undeclared name becoming a parameter that was never stored.

These began as tagged vectors — the classic portable trick, and what a
shim on someone else's Scheme has to do. The leaks were real rather than
theoretical: `vector?` said `#t` so a `vector?` branch shadowed the
predicate, `(vector-set! p 0 'x)` **destroyed the type** because the tag
was public, and a record printed as `[(record-type <point>) 3 4]`. The
second is what decided it — a type you can dismantle with an ordinary
vector write is not a floor to build on.

---

### `case-lambda`

R7RS-small (originally SRFI 16), now in the prelude. Several declared
arities, dispatching on the count:

```scheme
(case-lambda
  ((op)   …)      ; (d 'form)
  ((op v) …))     ; (d 'sample r)
```

The reason to reach for it over a rest argument is that a rest argument is
**permissive**: `(lambda (op . rest) …)` accepts any count and silently
drops the extras, so one argument too many gets no complaint.
`case-lambda` names the shapes it accepts and refuses the rest.

Clauses are tried in order, so put specific ones first — a clause with a
rest argument accepts every larger count and shadows anything after it.

Still missing from R7RS-small: `raise-continuable` and
`with-exception-handler`.

---

### `define-once`

Common Lisp's `defvar`, under a name that says what it does: bind only if
unbound, so re-running the file preserves the value.

```scheme
(define-once wgsl-signatures {})   ; a registry other files write into
```

The problem it names: `load` is textual inclusion with re-execution, and
the load graph is a diamond — a shared file runs once per **path**
through it. A top-level binding therefore has two possible lifetimes,
"this run of the file" and "the accumulated session", and a bare `define`
expresses only the first. For ordinary definitions the re-run is
harmlessly idempotent; for an **accumulator** it is destructive: the
second visit's `(define table {})` discards what other files registered
after the first.

**Reach for it when the value's contents come from outside the defining
file.** If the file computes everything in the value, use `define` — a
re-load should refresh it. The function spelling `(define-once (f x) …)`
is refused to keep that guidance structural: a function is file-owned by
definition, so there is nothing a once-only function could protect.

⚠️ The cost, inherited from `defvar`: editing the *initializer* of a
`define-once` does not take on a re-load. Restart the session to see it.

---

## 5. Generative functions

`lib/gen.scm`. A model is an ordinary procedure that calls `at` where it
makes a random choice:

```scheme
(define-gen (coin n)
  (let ((p (at :p (uniform 0 1))))
    (at :qs (batch (flip p) n))))

(sample (coin 4) seed)          ; draw everything, return a trace
(assess (coin 4) choices)       ; draw nothing, return (weight . retval)
```

Nothing about the model is transformed, declared, or annotated. `at`
yields; a driver on the other side of that yield decides what the choice
is worth and hands a value back.

### Why that is the whole trick

Systems doing this in a language without suspension have to synthesise it.
WebPPL CPS-transforms a subset of JavaScript; GenJAX decorates and traces
to a jaxpr; a source rewriter inserts the plumbing into the text. All of
them are buying **inversion of control**, and a coroutine already has it.

It also hides the generator. A model never mentions an RNG key — it isn't
*running* when a draw happens. It's suspended, and the driver holds the
key, the trace and the accumulators. That's a second transformation
(threading keys through every call site) made unnecessary by the same
mechanism.

⚠️ The cost is real: a live coroutine cannot be **inspected**. A jaxpr can
be compiled, differentiated or vectorised; this can only be run.

### `batch` is the vectorisation seam

`(batch d n)` converts a distribution's `fill`/`sum` into `sample`/`score`
at a larger shape, and is *itself* a distribution — which is what lets it
sit at an address. A driver never learns `fill` and `sum` exist.

It has no `fill` or `sum` of its own, stated in its constructor rather
than discovered by falling through, so `(batch (batch d n) m)` is refused:
a two-axis shape wants a different mechanism, not a nested one.

### `importance` — K samples, transposed

```scheme
(importance (coin 10) {:qs observations} 20000 seed)
;; => {:p       #<view f64 ✕20000 [0.6887 0.7328 …]>
;;     :weights #<view f64 ✕20000 [-9.288 -10.171 …]>
;;     :n 20000}
```

**Columns, not K traces.** K traces would be K maps plus one map per
address inside each. Measured at K=20000 on the coin model:

| | seconds | live |
|---|---|---|
| columnar | 0.120 | **0.41 MB** |
| K traces | 0.256 | 10.9 MB |

27× the memory, and the ratio grows with the number of addresses — the
coin model has one unconstrained address, so it is the *kindest* case for
the map version. Nothing at the call site fixes that; it is structural.

A **constrained** address gets no column, because its value is the same
for every sample. Storing K copies of it would record a fact already held
in the constraints.

**Two accumulators, and the asymmetry is the whole of it:** everything
goes into `score`, only constrained values go into `weight`. The
unconstrained draws come from the prior, so their density appears in
numerator and denominator and cancels. Adding them to the weight — the
obvious thing, since both branches compute a logpdf — gives a number that
is plausible, moves in roughly the right direction, and is wrong.

Sample *i* draws from `(rng-make i seed 0)`, so sample 37 is the same
whether you take 50 samples or 900, in any order.

⚠️ **Not vectorised.** This is a loop over K with one generator per
sample; only the *output* is columnar. At K=20000 that is 1819
collections, and 20000 bare generators alone cost 2500 — so the remaining
time is the per-sample fiber. Removing it means one pass over all K, which
needs distribution parameters that may be views rather than scalars, and
costs the ability to branch per sample.

Two shapes are refused rather than fudged: a **nested** generative
function (columns are keyed by a single address, so nesting needs
path-flattened keys) and an **unconstrained batched** choice (that would
want a K×n column, the two-axis shape we don't build).

### `define-gen` is not `@gen`

GenJAX's decorator performs a tracing transformation on the body. This
touches the body not at all — the coroutine does that work. It only makes
`model` a *constructor*, so `(model 1 2)` builds a generative function
rather than running one. What it buys is one name instead of two, and no
way to write the wrong one.

It does *keep* the body, which is not the same as transforming it: what
runs is the same closure it always was, and the datum rides alongside in
`gf-source` for anything that wants to **read** the model rather than run
it. That field is [§5c](#5c-staging-a-model-read-rather-than-run).

---

## 5a. Distributions on the host

`lib/dist.scm` holds the samplers as a faithful port of `lib/stat.wgsl`,
over a native Threefry core. The **log densities are no longer a port**:
`logpdf-normal`, `logpdf-uniform`, `logpdf-flip` and `logpdf-exponential`
are written once with `define-dual` ([§5c](#define-dual--one-definition-two-homes))
and compiled for both, so `lib/stat.wgsl` does not define them at all.

The samplers stay ported, and that is a real difference rather than work
left undone: each takes its generator explicitly here and finds it as
per-invocation private state on the device, so the two genuinely have
different signatures. `logpdf-gamma` and `logpdf-beta` stay here too —
both need `lgamma`, which WGSL has not got.

### Four that are not ports

`laplace`, `cauchy`, `categorical` and `dirichlet` were written here
first, so they have no WGSL order to follow and are grouped by
distribution rather than by capability — each one's sampler, score, fill
and sum read together. The ported section keeps its own arrangement
precisely because it has a second file to agree with.

Two of them reach the device and two do not, by the same rule as
everything else: a score that is a `define-dual` is a score the device
has. `laplace` and `cauchy` are ordinary arithmetic, so they stage —
**and they were never in `lib/stat.wgsl` at all**, which is the
dual arrangement working in the direction it was built for rather than
catching up with a port. `categorical` needs an indexed buffer read and
`dirichlet` needs `lgamma`, so both stay on the fiber path and a model
using them is refused by name.

Two of them stretched the vocabulary rather than extending it:

- **`categorical` takes a view for a parameter** — the first family whose
  parameter is not a scalar. Nothing in the distribution record had to
  change, because parameters are closed over and the record never cared
  what they were. The weights are **linear and unnormalised**; the score
  divides by the total, so a caller may hand over a buffer it is already
  using rather than run a normalising pass to satisfy the sampler.
- **`dirichlet` has a view for a value** — the first family whose draw is
  a vector. `batch` set that precedent, and `unsupported` already existed
  for the capabilities such a distribution cannot have. A Dirichlet is
  *not* n independent draws, so it is not a batch; it only has the same
  shape at the address.

`random-laplace` is a sign times an Exponential — two uniforms, side then
magnitude. Not the textbook inverse CDF, which diverges at both ends of
the unit interval where `rng-unit!` attains one of them; the usual answer
is to clamp `u` away from the ends, which alters the tails in the exact
region a heavy-tailed distribution exists to get right. Composing two
samplers that already handle their own endpoints fabricates nothing
instead.

```scheme
(define r (rng-make ptnum seed stream))   ; mirrors rng_init
```

| draw | score | fill a buffer | sum over a buffer |
|---|---|---|---|
| `random-uniform r low high` | `logpdf-uniform` | `fill-uniform!` ᴺ | `logpdf-sum-uniform` |
| `random-normal r loc scale` | `logpdf-normal` | `fill-normal!` ᴺ | `logpdf-sum-normal` |
| `random-flip r p` | `logpdf-flip` | `fill-flip!` ᴺ | `logpdf-sum-flip` |
| `random-exponential r lambda` | `logpdf-exponential` | `fill-exponential!` | `logpdf-sum-exponential` |
| `random-gamma r alpha lambda` | `logpdf-gamma` | `fill-gamma!` | `logpdf-sum-gamma` |
| `random-beta r alpha beta` | `logpdf-beta` | `fill-beta!` | `logpdf-sum-beta` |

Every row is also a **family** a model can name — `uniform`, `normal`,
`flip`, `exponential`, `gamma`, `beta` — which bundles that row's four
capabilities into one object that can sit at an address
([§5](#5-generative-functions)). The row is what you call directly with a
generator in hand; the family is what `at` is given. `exponential` and
`gamma` took a while to get theirs: `lib/dist.scm` had all four for both
of them well before the binding existed, so a model simply could not name
them, and nothing said why.

Which of them can also reach a **device** is a shorter list, and
[§5c](#what-may-sit-in-a-kernel-and-what-may-not) has it.

`random-gamma` **boosts below α = 1**. Marsaglia–Tsang's squeeze needs
α ≥ 1; below that `d = α − 1/3` goes non-positive, the acceptance test can
never pass, and a fabricated `1.0` came back — a plausible gamma value,
silently wrong for a whole parameter range. Their own remedy is
`Gamma(α) = Gamma(α+1) · U^(1/α)`, which also *lifts* the acceptance rate
because the squeeze always runs where it is good:

```
α:  0.5   0.33    0.2      0.05
    was:  1.0     1.0      1.0        ← fabricated
    now:  0.117   0.0225   1.67e-07
fabrications: 4 in 40,000 (was 8 in 2,000)
```

⚠️ The boost uniform is drawn **after** the core's draws. That's part of
the contract, not an implementation detail — `lib/stat.wgsl` consumes in
the same order and the two agree only while both do.

`beta` is `X/(X+Y)` over two Gamma draws, which is why it waited on that
fix: **Beta(½, ½) is Jeffreys' prior**, and before the boost both draws
fabricated `1.0`, so every sample was exactly 0.5 — a correct-looking mean
from a distribution that never varied.

⚠️ In `logpdf-sum-exponential` the last word is the **distribution**, as
in `logpdf-sum-normal` — not an instruction to exponentiate. "Exponential"
being both a distribution and an operation is a collision the family
pattern has to carry.

`lambda` is the **rate** in both `random-gamma` and `logpdf-gamma`.
`lgamma` is `std::lgamma`; WGSL has none, so a device version will be an
approximation and must be checked against this one rather than the
reverse.

ᴺ = native. The rest are derived by `generic-fill` from the scalar
sampler — a fill is the same loop every time, so only the hot ones are
written by hand and the others cost nothing to have. Every fill produces
exactly what drawing one at a time produces, consuming one uniform per
element in the same order; the tests assert that per distribution.

⚠️ `fill-unit!` is the **RNG layer's** raw (0,1) fill and takes no bounds.
`fill-uniform!` is the **distribution layer's** and takes the same
`low`/`high` its scalar sampler does. Conflating the two is what made
`fill-uniform!` silently ignore its bounds.

One family, `random-` / `logpdf-` / `logpdf-sum-`. There is no import
mechanism, so a Gen-ish layer above will rename these into whatever
clothing it likes; the point of the regularity is that it can do so
mechanically.

### Printing a view

```
#<view f32 ✕10 [0.49221453 1.9817239 1.7200769 0.87194514 …]>
#<view f64 ✕10 [0.4922145237111392 1.981723895011878 …]>
#<view f32 ✕0 []>
```

Truncated at `*view-print-length*` (default 8; `#f` shows everything).
Truncating costs nothing because `#<…>` has **no read syntax** — a printed
form `read` cannot consume was never a faithful transcription, so it is
under no obligation to show every element. What it *is* obliged to do is
not flood a REPL with a million floats.

An `:f32` view prints at **f32 precision** — the shortest decimal that
round-trips as a float, via `to_chars`' float overload. Its f64 widening
would show ten digits of precision that aren't in the storage, and invite
comparing host and device values past the digit where they can agree.

---

### Parameters may be columns

Every distribution parameter — `loc`, `scale`, `low`, `high`, `p` — is
either a **number or a view**, in both the fills and the sums:

```scheme
(logpdf-sum-normal ys 0 n means 0.5)   ; a mean per element, one scale
(rng-fill-normal! r ys 0 n means 0.5)  ; draw n points around a curve
```

A fitted curve is what forces this: every `y_i` is scored against `f(x_i)`,
so `loc` differs per element while `scale` does not. Requiring both to be
scalars makes scoring a curve a Scheme loop; requiring both to be views
makes you allocate a column of identical numbers.

`log(scale)` still hoists out of the loop when `scale` is constant, which
is the common case. A per-element scale pays a `log` each, as it must.

A view too short for the range is refused **once**, before the loop, rather
than read off the end per element.

⚠️ A scale column must be **positive** — reusing a means column that starts
at zero puts a division by zero in it.

---

### Buffer reductions

```scheme
(logpdf-sum-normal view start count loc scale)   ; -> one scalar
(logsumexp view start count)                     ; -> log(sum(exp(x)))
```

The shape that matters is M candidates each scored against N observations:
**M sums over N, not an M×N matrix.** The matrix is reduced along N
immediately, so materialising it would cost 40 MB at M=1000, N=10000 and
evict the cache for nothing. Only the inner dimension is native; the outer
stays an ordinary loop.

| | |
|---|---|
| one candidate, 10k observations, in Scheme | 2.35 ms |
| the same, `logpdf-sum-normal` | **9.8 µs** |
| 1000 candidates × 10k observations | **7.4 ms** |

`logsumexp` is native for **correctness**, not speed — the counts are
small. The naive form overflows above ~709 and underflows to zero below
~−745, and log-weights live out there routinely:

```
logsumexp  -799.56          naive  -inf
```

Out-of-support values give a true `-inf`, matching the scalar `logpdf-*`
and the device. That is the right answer downstream: `(+ -inf x)` is
`-inf` and `(exp -inf)` is `0`, so an impossible candidate gets exactly
zero probability. Where `-inf` differs from a merely-large negative —
`-inf` minus `-inf` is NaN — the NaN is honest, since the ratio of two
impossibilities is undefined.

The generator is explicit. On the device it is per-invocation private
state and so implicit; here many streams may be alive at once, and hiding
which one a draw came from is the confusion the whole design avoids.

### Why a port and not a better design

Threefry is a pure function of (counter, key), so a host draw at a given
counter is the draw the *device* makes at that counter. That makes the CPU
path an **oracle** for the shader — run a model both places and the
answers should agree. An improved algorithm would forfeit exactly that.

**How close is "agree":** the uniform stream is bit-identical, because
`rng-unit!` is `m / 2²³` for `m` the top 23 bits, which is exact in f32
and f64 alike. Everything downstream is not — the device evaluates these
polynomials in f32 and the host in f64 — so samplers agree to about f32
precision. A larger discrepancy is a bug; a last-few-bits one is
arithmetic.

⚠️ `lib/threefry.scm`'s `u32->unit` divides by 2³². That is a **different
number** from `rng-unit!`, and using it where you meant a device-matching
draw is silent.

### Splitting

```scheme
(rng-split! r)   ; consume one block from r, return a generator keyed on it
```

This is what `jax.random.split` does, and for the same reason: **split *is*
Threefry** — the parent's output words become the child's key. Coordinates
and splitting are one primitive with two ergonomics, not rival designs.

Use coordinates when you know the address (a point index, a batch
element).
Use split when descending into something that should not need to know what
its parent or siblings did — a nested generative function, say.

What split buys: a child is insulated from its siblings' **draw counts**.
Change how many values one sub-computation consumes and the others do not
move. Threading one generator through gives you the opposite, and editing
one submodel silently reshuffles every later one.

⚠️ What it does **not** buy: a child's key still depends on how many splits
came *before* it, so adding a sub-computation moves every later sibling.
Hashing an address into the counter would fix that and is strictly
stronger — at the cost of deciding how address paths hash into 32 bits and
what happens on collision. Split is the smaller commitment, and addressing
remains an additive change rather than a rewrite.

### Bulk draws

| | rate |
|---|---|
| `rng-fill-unit!` (native) | **185 M/s** |
| `rng-fill-normal!` (native) | **50 M/s** |
| `rng-unit!` in a Scheme loop | 9.9 M/s |
| normals in a Scheme loop, native `erfc` | 3.9 M/s |
| Threefry in Scheme (`lib/threefry.scm`) | 0.55 M/s |
| normals with `erfc` in bytecode too | 0.51 M/s |

`erfc`, `inv-erfc` and `inv-erf` are native as well. Every normal is
inverse-CDF, so that polynomial *is* the cost of a normal — it was the
whole 185× gap between bulk uniforms and bulk normals. `lib/dist.scm`
keeps Scheme transcriptions as `erfc/reference` and friends, readable
beside the WGSL and asserted against the natives by layer 22.

Fill a typed buffer rather than building a list: a million-element vector
costs 262 µs per collection because every slot might be a pointer, while
the equivalent `bytes` buffer costs 2.75 µs because the collector doesn't
walk it at all.

Only the RNG **core** is native. Measurement is why: of the 0.181 s the
Scheme version took for 100k draws, only a quarter was subr dispatch — the
rest was the round loop's `quotient`/`modulo`/`vector-ref` in bytecode. So
the core moved down and every distribution stayed in Scheme, next to the
WGSL it mirrors.

---

## 5b. Driving the REPL from an editor

`vx-scheme` runs its interactive loop when stdin is a terminal, and
evaluates stdin as a script otherwise. `--repl` (or `-i`) forces the
interactive loop regardless — for an editor's inferior process, a wrapper
script, or a harness.

Worth knowing because the default's failure is **silent**: a REPL that
reads to EOF before answering is indistinguishable from a hang.

Emacs works today with no vxs change, since `make-comint` allocates a pty:

```elisp
(setq scheme-program-name "/path/to/vxs/src/vx-scheme")
(require 'cmuscheme)
(setq comint-prompt-regexp "^\\(vxs\\|\\.\\.\\.\\)> *")
```

`M-x run-scheme`, then `C-x C-e`, `C-M-x`, `C-c C-r`, and `C-c C-l` — the
last works because `load` is real. Anything connecting by pipe instead
wants `--repl`.

⚠️ Errors carry no `file:line`, so `next-error` has nothing to parse. See
§6.

---

## 5c. Staging: a model read rather than run

`sample` and `assess` drive a model by *running* it. Staging does the
other thing — it reads the source and produces a description of the
log-joint, which two backends turn into a number on the VM or into kernel
code for a device.

There is no tracer here, and the reason is not cleverness. A tracer
exists because Python cannot hand a decorator the body of a function as a
manipulable datum, so the only way to learn what a computation does is to
run it and watch — which costs trace memory proportional to what the code
**did**, in order to recover what it always **said**. `define-gen` was
handed the source and kept it, so the cost is proportional to the length
of the model text, and the reading happens once whatever N and K are.

```scheme
(load "lib/stage.scm")

(define-dual (curve-elem (x :f32) (a :f32) (b :f32) (c :f32))
  (+ (* a x x) (* b x) c))

(define-gen (curve xs sigma npts)
  (let* ((a (at :a (normal 0 1.5)))
         (b (at :b (normal 0 1.5)))
         (c (at :c (normal 0 1.5))))
    (at :ys (batch-i npts (j)
              (normal (curve-elem (view-ref xs j) a b c) sigma)))))

(define st (stage (curve xs 1.0 10)))
(staged-logpdf st {:a 2.0 :b -1.0 :c 0.5 :ys ys})   ; a number, on the VM
(staged-kernel st)                                   ; kernel code, for a device
```

### The tax

A model pays to enter this domain, and the whole bargain is that the tax
is small, flat and visible — paid once, at the definition, by the person
who has the knowledge. In exchange nothing is discovered at run time that
could need machinery to cope with.

- **Data arrive as parameters, not globals.** Staging has no `eval` and
  cannot read a global, so `(stage (curve xs 1.0 10))` gets `xs` because
  the call supplied it. This is also why a staged snapshot cannot go
  stale: there is no captured global left to change.
- **Helpers are registered**, via `define-dual`. A call is a lookup, and
  an unregistered name is refused rather than followed.
- **Repeated choices use `batch-i` or `scan-i`**, which write the
  per-element structure down instead of handing over a finished column.

### `define-dual` — one definition, two homes

```scheme
(define-dual (name (arg :type) ...) body)
```

Emits an ordinary Scheme procedure *and* a kernel function, from one
body. Note the asymmetry, because it is the entire idea: the host side
splices the body **unquoted**, so Scheme evaluates it; the kernel side
splices it **quoted**, so the compiler reads it as text in another
language. Same datum, two fates, no second copy to drift.

The body must therefore be valid in both. Some names differ between the
languages and the compiler bridges what it can — `expt` compiles to
WGSL's `pow` — but a kernel builtin with no Scheme meaning will type-check
here and fail as an unbound variable on its first call there.

What it does **not** buy is numerical agreement: the device computes in
f32 and the VM in f64. That gap is deliberate — see
[§5a](#why-a-port-and-not-a-better-design) on why the more precise side is
the oracle — so a comparison across it is a *measurement*, not a test to
be tightened until it passes.

`define-dual` is also how the log densities are defined. There is no
hand-written `logpdf_*` in `lib/stat.wgsl` any more; adding one would be
a second copy of something that no longer has a first.

### `batch-i` maps, `scan-i` scans

```scheme
(batch-i n (j) dist-expr)                       ; the j-th from j alone
(scan-i  n (j) ((name init step) ...) dist-expr) ; the j-th from the (j-1)-th
```

`batch` takes one distribution and repeats it, so a varying parameter has
to arrive as a *column* — the finished result of a loop that ran
somewhere else, which nothing can read. These write the structure down.

`scan-i` carries state: the j-th observation is scored against the state
as it stands, and the state then advances by the step expressions. **All
the steps see the old state** — they are evaluated together and the
components update at once, which is `let` and not `let*`. That is not a
detail: threading them would silently turn an explicit integrator into a
semi-implicit one, which is a different method that still converges and
still looks plausible.

Both run on the fiber path like any other distribution, and the readable
shape has a price there — see [§6](#batch-i-allocates-a-distribution-per-element--noted-not-scheduled).

### The intermediate form

A plain datum on purpose. It is a format, not an object, so something
other than the reader could emit one and both backends would still
consume it.

```
staged = {:choices ((addr scalar) | (addr batched n) ...)
          :buffers ((name . view) ...)
          :terms   (term ...)}

term   = (score dist expr)
       | (sum-over n idx term)
       | (scan-over n idx ((name init step) ...) term)
```

Terms are summands of the log-joint **in source order**, and that order
is load-bearing: float addition is not associative, so it is what lets
the two backends agree exactly rather than approximately.

There is no separate notion of "observed" in here. A constrained address
and a latent one differ in where their value comes from at evaluation
time, not in how they are scored, and keeping that distinction out is
what lets one staged object serve comparison, importance and
rejuvenation alike.

### What may sit in a kernel, and what may not

| | |
|---|---|
| distributions that stage | `normal`, `uniform`, `flip`, `exponential`, `laplace`, `cauchy` |
| refused | `gamma`, `beta` — both need `lgamma`, which WGSL has not got |
| refused | `categorical` — its weights want an indexed buffer read, which is a gather |
| refused | `dirichlet` — needs `lgamma`, and its value is a vector |
| state per `scan-i` | at most **fifteen** components |

The ceiling is the device's, not a shortcut: `fold-i` carries one
accumulator, that accumulator holds the running score as well as the
state, and the widest register bundle WGSL has is a 4×4 matrix — sixteen
slots, less one for the score. The accumulator widens as the state does,
from a `vec2` up to a `mat4x4`, and the packing is arithmetic rather than
a table: element *k* lives at column *k*/4, row *k*%4.

A matrix here is a state **bundle**, not linear algebra. That is why
`lib/wgsl.scm` exposes a constructor and `mat-col` and no matrix
multiply: the operation exists in WGSL, means nothing for a bundle of
state, and a checker that accepted `(* state1 state2)` would be accepting
an expression with a type and no meaning. It is the move `:quat` already
makes — the storage type carries the intent, the type system carries the
shape.

Fifteen is a great deal more than a trajectory usually needs: a pendulum
takes two, a double pendulum four. The point of the width is not the
width. It is that an algorithm whose working state fits in the fold has
no reason to reach for per-element scratch, and scratch was the thing
pulling toward wanting a terminal that is not a `point`.

### Everything else is refused by name

A model that cannot stage is not broken. It stays on the fiber path,
which is the general case and remains the oracle — so the refusals say
what was wrong and where, rather than miscompiling into something
plausible: a global, an unregistered helper, a distribution with no
device score, an address used twice, a computed index (that is a gather,
with different in-place safety), arithmetic on a batched choice, a buffer
indexed by a state value, and a generative function with no source to
read.

One refusal is worth singling out because it is easy to misread as a bug.
A **declared** function is hand-written WGSL and has no host meaning at
all, so a model calling one stages happily for the device and is then
refused by `staged-logpdf` — which is right, because such a model could
never be checked against `assess`, and that check is the point.

---

## 5d. Rejuvenation: Metropolis-Hastings over a staged log-joint

`lib/mh.scm` moves a particle's latents and keeps or discards the move on
a log-joint ratio. It is the rejuvenation half of the SMC pipeline and the
route to it that **asks nothing of the model but its log-joint** —
[§5e](#5e-conjugate-structure-read-from-the-ir) refuses a model whose
conditional has no closed form, and this one does not care. Every model
that stages can be rejuvenated.

```scheme
(load "lib/mh.scm")

(staged-logjoint-fn! st 'logjoint)          ; => (a b c), the parameters
(staged-mh-body st 'logjoint 'mhstep 'taken) ; => an expression ending in point
(mh-sweep st choices step rng)               ; => (choices . accepted?)
(mh-run   st choices step rng n)             ; => (choices . acceptance-rate)
```

### The log-joint becomes a function

An accept ratio needs the log-joint at **two** parameter values. Inlining
the staged expression twice would emit its likelihood fold twice — two
reductions, and twice the work for an answer differing in three
arguments. `wgsl-define-fn!` takes the staged expression as a body and the
scalar choices as parameters, so the fold is emitted once inside
`fn logjoint(...)` and the sweep calls it twice.

That costs nothing to arrange, because **the staged kernel's free names
already *are* the scalar choices** — the expression is a function of them
in everything but name.

### Sweeps are substeps

`gpu-wrangle!`'s substep count runs the kernel N times in one submit and
hands each run its own value of `step`, which the preamble gives to
`rng_init` as the stream index. ⚠️ Sweeping any other way replays the
identical proposals — the hazard [§4](#substeps) warns about, and the one
that makes a chain look busy while going nowhere.

### The `let` wraps the terminal, and that is not style

The terminal form compiles each of its attribute arguments **separately**,
so a proposal computed inside them is computed once per attribute. Since a
draw is stateful, each coordinate would then get a *different* proposal
and the particle would fly apart while looking entirely plausible. One
`let` outside the `point`, one proposal, one decision, N writes of it.

### What the error measurement licensed

The device's log-joint carries a small systematic bias against an f64
evaluation ([§6](#-the-staged-kernel-answered-by-a-real-device)). An
accept ratio is a **difference** of log-joints at nearby parameters, so a
bias common to both largely cancels. That is the benign half of the
measurement, and the half this rests on.

### How an MH kernel is tested at all

A chain cannot be compared against an expected value sample by sample —
its correctness is a claim about what it converges to. So the subject is a
model with one latent, whose conditional is therefore its posterior, and
whose posterior `lib/gibbs.scm` gives in closed form. Twenty thousand
sweeps against a Normal(0, 2) prior and twelve observations land the mean
within **4e-5 of a posterior standard deviation** and the spread within
0.4%, at a 38% acceptance rate.

The step size reaches the kernel as a live wrangle parameter rather than a
baked constant, so it can be tuned against the acceptance rate without
recompiling — which is the one diagnostic no inspection of the samples
reveals. Both failure modes are checked: a step far too large is almost
never accepted, and one far too small is almost always accepted and goes
nowhere.

### A joint proposal, for now

Every scalar choice moves at once under one accept decision.
Componentwise would mix better on a correlated posterior — curve
coefficients are strongly correlated — and costs one call per address per
sweep rather than two, which making the log-joint a function already makes
affordable. Not done because a joint chain is the smaller correct thing.

---

## 6. Known gaps and open work

What is wrong, what is missing, and what was decided about each. Kept here
rather than in a separate file so that a rule and its known exceptions stay
next to each other.

Nothing here is scheduled. **Decided** marks a course that has been
settled; the rest are candidates.


### Correctness gaps

#### ✅ `guard` is compiled inline — **done**

`guard` used to desugar to a subr that ran its body through `call_closure`
inside a C++ `try`, putting native frames above the fiber so it could not
suspend. It now compiles inline like `unwind-protect`: `OP_PUSH_HANDLER`
parks a record on `Fiber::handlers`, the body runs in the fiber's own
dispatch loop, and `raise` finds its handler by searching that stack, with
one `catch` per **dispatch loop** rather than one per guarded call.

What that bought: `(yield)` and a host-settled `(touch)` both work inside
`guard`, closing §2's worst row — wrapping a GPU call in an error handler
is the obvious thing to write and was exactly what could not work. Guarded
code also got **39% faster** (0.503s → 0.308s on a tight loop), since
there is no nested dispatch and one closure allocation per entry instead
of two.

Still to do, and the reason this is stage one of two:

- **The single catch does not yet cover native VM errors.** A raise
  becomes a C++ `RaiseEscape` and the dispatch loop catches it, so
  anything routed through `raise`/`error` is found. Native contract
  violations set `f.state = Error` and return `StepResult::Error`
  instead, bypassing the handler search entirely — which is the item
  below, now a much smaller change than it was.
- **A handler at or below `stop_at_depth`** belongs to an outer dispatch,
  with native frames between; the catch rethrows and the enclosing
  dispatch resumes the search. Correct, but it means a raise crossing
  `map` still unwinds through C++.

`map`/`for-each`/`force`/`dynamic-wind` are unchanged and are not the same
problem: they genuinely call closures from native code, so there is no
body to compile inline.

#### Two contract-violation mechanisms — **decided: converge, but downward**

This entry previously read "`guard` cannot catch native VM errors —
decided: worth fixing", and proposed converging every site on the
catchable path. **That conclusion is withdrawn.** The diagnosis it rested
on was right; the remedy was backwards.

The diagnosis stands: two mechanisms that grew apart.

| | behaviour | catchable | sites |
|---|---|---|---|
| `raise_contract` (`vx_vm.h:741`) | error object → `in_flight_raises` → `throw RaiseEscape` | ✅ | 8 |
| legacy | sets `current_fiber->state = Error`, writes `error_message`, returns unspecified | ❌ | ~36 |

Drift rather than intent: `raise_contract` is used only by the most
recently written primitives (bytes, views, vectors), the two print in
different formats (`[VM Error] car: …` vs `bytes-view: …`), and the legacy
sites are one five-line block copy-pasted — a template, not a judgement
about what should be recoverable.

**Why the remedy reverses.** All eight `raise_contract` sites are contract
violations: a negative length, a sealed buffer, an unknown type keyword, an
index out of range. Every one is the same category as `(car '())` — R6RS's
`&violation`, a bug in the calling program. [§3](#3-errors-what-is-catchable)
argues those should not be catchable in place and that the crossing point
is a fiber boundary. Converging *upward* would spread inline catchability
to another 36 sites and make `(guard (e (#t #f)) …)` a plausible way to
swallow real defects.

So: converge downward, and make the printed format consistent while doing
it. The capability being removed has no principled users — a genuinely
**environmental** fault (a device lost, a file missing) *should* be
catchable, but no vxs primitive currently raises one. Those arrive through
futures, where `touch/or-error` already handles them.

Not urgent. The inconsistent *message format* is the part that costs
something today.

#### A dual body's `let` means two things

The kernel language's `let` binds sequentially (`let*` is the same form
there), documented in [§4](#bounded-folds). For a pure kernel that is a
language choice; for a `define-dual` body it is a latent divergence,
because the same datum also runs as real Scheme on the host, where `let`
binds in parallel. The silent case needs a binding list that both
shadows a parameter and references it: in
`(let ((sigma 2.0) (z (/ x sigma))) …)` with `sigma` a parameter, the
device's `z` divides by 2.0 and the host's by the parameter. Both run.

lib/stage.scm had the same divergence against the fiber path and now
stages `let` in parallel (layer 24 pins both semantics). The kernel
compiler cannot simply follow, because sequential `let` is documented
behaviour that hand-written kernels may rely on. The candidate fix is
narrower: at `define-dual` only, refuse a `let` whose init mentions a
name bound earlier in the same list, and say `let*`.

#### Faults carry a string, not a structure

`(touch child)` on a fiber that died from `(car '())` hands the supervisor
an error object whose message is prose. You can report it; you cannot ask
what kind of fault it was.

R6RS names the shape worth copying: `&who`, `&message`, `&irritants` — the
procedure, what happened, and with what. The blocker is not GC (`error`
already allocates at its raise point, with its arguments rooted on the
fiber's stack). It is that `Fiber::error_message` is a `std::string`, so
there is nowhere to put an object, and that contract violations never
raise at all — they set a flag and return a sentinel, which the dispatch
loop notices at two sites.

This is **independent of catchability**, and only this half is worth
wanting: it improves supervision without making bugs catchable in place.
One caveat if it is ever done — a fault site that allocates assumes the
heap can still allocate, so the string wants keeping as a fallback for the
case where the fault *is* exhaustion.

#### The reader has no `#x` / `#b` / `#o` literals

`(string->number "FF" 16)` works; `#xFF` is read as a symbol and fails as
an unbound variable. Small, self-contained, and the error message points
nowhere useful.

#### ✅ Arithmetic refuses non-numbers — **done**

This entry used to record the silent-zero behaviour as **undecided**,
entangled with keeping `+` first-class and un-opcoded for speed, with any
fix owing a benchmark answer first. The benchmark answered: **the
entanglement was a misdiagnosis**. There are no arithmetic opcodes to
protect — `+` is an ordinary subr — and `as_real()` already executed both
tag tests on every call, discarding their answer. The silent zero was
never buying speed; the check changed only the fall-through arm, and
measured within noise on a tight arithmetic loop (0.220s vs 0.220s over
~12M numeric subr ops).

What made it worth doing now rather than eventually: comparisons. A wrong
*number* propagates somewhere visible; `(< 'typo x)` was a wrong *boolean*
that silently picks a branch, `(= 'a 'b)` was `#t`, and `(max 'a -5)`
returned the symbol. In a generative model those become a confident
posterior around the wrong answer.

Every numeric subr now checks, via `VM::numeric_contract` — the
fiber-state path, per the converge-downward decision above. The integer
division family also refuses a zero divisor (`(remainder 5 0)` was `0`);
`(/ x 0) → inf` is kept as IEEE semantics. See §3 for the behaviour as
documented.

#### ✅ `Rooted` — the bug class as a type

Six rooting bugs, one shape: *take a value out of a rooted place into a
bare C++ local, then allocate.* Two of them sat under comments warning
about that exact hazard, and one was written three lines below a fix for
the same thing in the same function. That is the argument for a type
rather than a rule.

A `Rooted` registers a temp root **at its own address** on construction
and drops it on destruction, so its protection lasts exactly as long as
the object does. That one sentence covers both uses — they are not two
mechanisms:

```cpp
Rooted err(heap, …);                                  // named: rooted for the scope
heap.make_error_object(heap.make_string(msg), {})     // temporary: rooted for the
                                                      // full-expression
```

Every allocator returns one, which is why the second line is safe with
nothing written at the call site: `make_string`'s temporary lives until the
`;`, so it is still rooted while `make_error_object` allocates. That line
was a use-after-free, and avoiding it previously took four
`push_temp_root`/`pop_temp_root` lines.

⚠️ **A guard cannot be a parameter type**, and that was the first design
tried. C++ evaluates arguments before the callee runs, and nothing can
root them *during their own evaluation* — so `f(make_string(a),
make_string(b))` is unsafe however `f` declares its parameters. Guarding
the **result** is what works. Immediates (`nil`, integers) stay plain
`Value`s; there is nothing to root.

⚠️ **What it does not cover.** Converting to a bare `Value` discards the
guard:

```cpp
Value v = heap.cons(a, b);   // the temporary dies at the semicolon
heap.cons(x, v);             // v is unrooted across this allocation
```

That is `subr_map`'s bug shape, and `Rooted` is silent on it — name the
local `Rooted` instead and the protection lasts the scope. **31 sites**
currently bind an allocator result to a bare `Value`; each is safe only
while nothing allocates before the value is installed. Making the
conversion explicit would catch them at compile time, at the cost of
touching all 106 `cons` sites — undecided.

Non-copyable and therefore non-movable: the root holds `&v_`, so a copy
would register two roots and a move would leave one pointing at a
moved-from shell. It also makes `std::vector<Rooted>` a compile error,
which matters because reallocating the buffer would invalidate every
registered pointer. C++17's guaranteed elision for prvalue returns is what
lets an allocator still `return Rooted(…)` with no move. The destructor
**truncates to its own depth** rather than popping, so it composes with
the raise path.

**Transfer into a rooted home is the one move that type-checks.** The
open question this design left was how to hand a temporarily-rooted
object to its permanent place. In general that cannot be typed: whether
a destination is safe is a property of *reachability*, not of the
destination's type. But there is one destination where it is a property
of the type — another live `Rooted` — so that case, and only that case,
is allowed:

```cpp
Rooted res(heap);
res = heap.cons(out, res);   // takes the value; res's slot is already a root
Rooted other(heap);
res = other;                 // ✗ selects the deleted copy-assign
```

Move-assignment takes the value and nils the source, which never touches
the root table: both slots were registered at construction and stay
registered until their own destructors. It stays non-move-*constructible*
(declaring a move-assign suppresses the implicit move constructor), so
`std::vector<Rooted>` is still rightly a compile error. The effect is
that a `Rooted` can be *emptied into* a root but never *aliased* by one.

⚠️ This was found by a **compiler upgrade**, not by a test. Older clang
accepted `res = heap.cons(out, res)` by routing it through `operator
Value()`; the current one applies overload resolution correctly — binding
a `Rooted` prvalue to `const Rooted &` is an identity conversion and so
beats a user-defined one, which selects the deleted copy-assign. The
line should never have compiled. Worth keeping in mind when a
`=delete`-based design appears to work.

`temp_roots` moved from the VM to the **Heap** to make this possible —
`VM` is an incomplete type in `vx_heap.h`, so a guard whose constructor
could not be inlined would have put a function call on every `cons`. The
VM keeps forwarding methods, so **no call site changed**: 106 `cons` sites
and 19 allocators converted with zero edits outside the two headers.

**Measured free.** Ten million conses: 0.76s median before, 0.72–0.74s
after. A `malloc` per cell dominates a vector push by roughly 20×. ⚠️ That
margin depends on `malloc`-per-cell — move `ObjCons` to a bump allocator
or a slab and this wants re-measuring, not assuming.

**Also not covered**, honestly: an unrooted `std::vector<Value>` whose
elements are passed *as* arguments, and a `Fiber` (not a `Value` at all).
Those remain discipline plus `--gc-stress`.

#### `--gc-stress`, and the bug class it makes visible

```bash
./src/vx-scheme --gc-stress <script>      # collect before EVERY allocation
```

`--gc-threshold` was already here and is **not** aggressive enough: after
each collection the threshold becomes `max(min_gc_threshold,
bytes_allocated * 2)`, so once the heap is large a low floor stops
mattering. That is why one rooting bug fired once every 1621 iterations,
moved whenever unrelated code changed, and moved when *instrumented* —
`load` compiles a whole file before running it, so appending a marker to a
suite shifted the heap before execution and the failure went away.
Bisecting by perturbation is self-defeating on this class.

Collecting unconditionally makes it deterministic. A value live across an
allocation and invisible to the collector dies at its **first**
opportunity. Note where the collection lands relative to the constructor:
`allocate()` collects *before* building the object, so arguments a caller
passed by value are exactly the values under test. Applied after the
prelude loads. O(heap) per allocation — a test mode, nothing else.

It turned "layer 06 crashes somewhere, sometimes, depending on what ran
before it" into "layer 06 crashes in five milliseconds, alone, at this
line".

**THE SHAPE, four times over.** Take a value out of a rooted place into a
bare C++ local, then allocate.

| where | the rooted place | what freed it |
|---|---|---|
| `subr_map` | — (a subr's bare return) | `heap.cons(out, res)` |
| `run_pending_winders` | `f->winders` | the cleanup's `call_closure` |
| the guard-catch path | `f->handlers` | `run_pending_winders` |
| `push_closure_frame` | — (an unrooted `std::vector`) | `heap.cons` folding the rest list |

The last one is the one to read twice: `rest_list` **was** rooted and the
*elements* were not, so a variadic procedure's rest argument arrived the
right length holding somebody else's data. Nothing crashed.

Also fixed in the guard-catch path: `truncate_temp_roots` ran *after*
`push_temp_root`, truncating to a depth from before the frame existed and
discarding the root just taken — so the matching pop took someone else's.
**Truncate the dead entries first, then establish yours.** `call/cc`
already had this right and says so; the guard path did not.

**Where the suite stands under stress.** 26 of 28 layers clean.
`28_mh` exceeds a two-minute budget rather than failing — twenty thousand
MH sweeps with a mark-sweep per allocation. And one real bug is open:

#### The collector toolkit

```bash
--gc-threshold N   # lower the collection floor (weak: the floor stops
                   # mattering once the heap grows, see collect_garbage)
--gc-every N       # collect every Nth allocation
--gc-stress        # collect before EVERY allocation (i.e. --gc-every 1)
--gc-poison        # quarantine freed objects and report what they were
```

```bash
make -C src test-gc-stress     # the whole suite, one collection per allocation
```

Deliberately **not** part of `make test`: it costs about seven minutes
against under two seconds. Run it before merging anything that touches the
VM, the scheduler, or a root set — six rooting bugs were found this way and
none of them was reachable at the default threshold.

`--gc-every` exists because `--gc-stress` is O(heap) per allocation: fine
for a five-line repro, about seven minutes for the suite, and far too slow
to run *underneath* another instrument. At N = 100 the suite takes seconds.
That said, every bug found so far needed N = 1 or close — N = 5 was already
clean — so the knob's real use is running a sanitizer over a **reduced**
repro rather than over the suite.

`--gc-poison` keeps a freed object's header allocated with its type set to
`Poisoned`, and logs what it was, which collection took it, and the shape
of the root set at that moment. `format_value` then prints
`#<freed: a Cons freed by collection 9344>` instead of `#<unknown>`.
`Heap::warn_if_dead` is the write-side companion: a *read* of a
quarantined object is caught by its type, a *write* just scribbles on a
corpse — and with real freeing it scribbles on malloc's freelist, which
surfaces as an unattributable trap inside `libsystem_malloc`.

⚠️ **Poison MASKS a use-after-free**, because nothing is recycled. If a
failure disappears under `--gc-poison` but not `--gc-stress`, that is
itself the diagnosis: something is being used after free, and what broke
was the reuse.

**Guard malloc is the cheap instrument, not ASan.** ASan on this VM is
roughly a thousand times slower — unusable even on a tight loop. Reduce
the repro first, then:

```bash
DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib ./src/vx-scheme --gc-stress <repro>
```

which faults on the protected page at the moment of the bad access. That
is what finally named `generator_fault`, in one run, after three wrong
guesses.

#### ⚠️ Dead code can make this class of bug disappear

Worth its own entry, because it cost a day and it will happen again.

Two guard branches were added to `~ObjGenerator` and `retire_fiber` on a
double-free hypothesis. The suite went green under `--gc-stress`, three
runs in a row — **and neither branch ever executed.** Dead code cannot fix
a use-after-free; the guards had merely moved allocation timing enough
that the collection landed elsewhere. Removing them brought the crash
straight back, at the same line.

The same thing defeats instrumentation *of the Scheme*: `load` compiles a
whole file before running it, so appending a marker to the end of a suite
shifts the heap before execution and the failure vanishes. And it defeats
bisection by subsetting: adding or removing a suite moves the fault.

So the rule, which is now written down rather than learned twice:
**change nothing while hunting one of these.** Reduce the repro until it
fails in hundredths of a second, then put an instrument *underneath* it —
guard malloc, or the poison log — rather than editing the program. Any
"fix" that was not preceded by an explanation of the mechanism should be
suspected of being a relocation.

#### ✅ Two more release-before-install bugs — **fixed**

Both the same shape as `subr_map`'s, which makes five instances of one
mistake, three of them specifically *releasing a root before installing
the value it protects*.

**The generator's rest list.** `generator`'s primitive builds a variadic
procedure's surplus arguments into a list, and the comment above it always
said the list was "rooted across" the generator allocation. The code
released the root one line *before* `make_generator`. Under `--gc-stress`
the list was collected every time, and the second `resume` of a variadic
generator read a freed cons. The root now drops after the list is on the
child fiber's stack.

**`generator_fault`'s error object**, written as

```cpp
vm.heap.make_error_object(vm.heap.make_string(msg), {});
```

The string is a bare C++ temporary, reachable from nothing while the
vector that is about to hold it is allocated. `format_raised_value` then
read it through `display_value`. It was the **only** nested
`make_X(make_Y(...))` in the codebase — and it is exactly the shape a
rooted-`Handle` parameter would refuse outright, since a temporary cannot
be a `Handle`. The best single argument for that refactor found so far.

#### The poison log, and the nested-future crash

```bash
./src/vx-scheme --gc-stress --gc-poison <script>
```

Poisoning **quarantines** a freed object instead of releasing it: the
header stays allocated, its type becomes `ObjType::Poisoned` so any later
use falls through every switch, and a side log records what it *was* and
which collection took it. The point is that a dangling pointer keeps
referring to something the collector owns, so the question stops being
"what died" — which malloc has already destroyed the evidence for, and
which is why the same crash printed `#<unknown>` one run and `#<fiber>`
the next — and becomes **who was holding it**. It leaks by construction;
a debug mode for a small repro, nothing else.

On `testcases/repro/fiber_pump_gc.scm` it turns a bare SIGSEGV into:

```
run_dispatch entry: frame 0 of 1 holds a Closure freed by collection 3
  [current=set active=1 chain=1]
  | fiber in_active=NO is_current=yes in_current_chain=yes
    state=1 backing_future=no
```

Read that as: a fiber in **Running** state (1), holding a single frame
whose closure was collected, with **no backing future** and **not in
`active_fibers`** — and at the moment of the free the
`current_fiber → parent_fiber` chain had length **1** and did not contain
it.

**Which is a violated invariant, not just a rooting gap.** `mark_roots`
reaches a fiber only through `active_fibers` or that chain, so a Running
fiber in neither is unreachable. And `is_dispatching` asserts exactly the
property that fails here —

> step_fiber links each fiber to the one it interrupted (parent_fiber) and
> makes itself current, so the chain from current_fiber IS the set of
> fibers that are presently running.

— which means the same hole is a latent use-after-free in the *scheduler*
independent of the collector: `step_all_active_fibers` skips
`is_dispatching(f)` precisely so it never re-enters a live dispatch, and a
Running fiber missing from the chain would not be skipped.

So the fix belongs in whoever runs a fiber without linking it, not in
`mark_roots`. Two candidate windows, neither yet confirmed: `call_closure`'s
throwaway `Fiber scratch`, which holds a frame from `push_closure_frame`
before `step_fiber` makes it current; and any path that re-enters
`run_dispatch` on an already-running fiber without going through
`step_fiber`. Three hypotheses have already been refuted by experiment —
parent chains of active fibers, the winder pop, and nested scratch
orphaning — so the next step is a breakpoint on `step_fiber`'s entry
logging every transition, not more reading.

#### ✅ `map` dropped a subr's result into the cons that stored it — **fixed**

Found from the other end entirely. Pointing `lib/mh.scm` at the pendulum —
the first model on that branch whose latents are not polynomial — raised

```
[stage "unbound index" th]
```

from `staged-value`, several layers from the cause. `subr_map` called its
function and consed the result onto the chain it was building:

```cpp
out = vm.call_subr(...);        // a bare Value; nothing else refers to it
res = vm.heap.cons(out, res);   // allocates, so this can collect `out`
```

A **closure's** result survives, because the callee leaves it in a fiber
stack slot that `mark_fiber` reaches. A **subr's** does not. `cons` is a
subr, so `(map cons names s)` — which `staged-term`'s `scan-over` branch
uses to build its state bindings — produced a list of the **right length
whose elements were not pairs**. Nothing crashed; a caller checking only
the length saw nothing wrong. `out` is now rooted across the cons on both
the two-argument and n-ary paths.

**Two things hid it for a long time.** The input lists must be *freshly
allocated*: a quoted literal lives in the constant pool and stays
reachable regardless, which is what every existing test used. And the
mapped function must be a *subr*: map a closure and the bug is invisible.

**What made it look like a race, and was not.** The failure iteration
moved between runs and between unrelated edits, so a first pass called it
allocation-timing noise and gave a confident wrong diagnosis twice —
once blaming the alist's reachability, once "chained `map-copy` plus
native RNG calls", with seven controls that were passing on luck. Asking
*what* was wrong with the result rather than *whether* it was wrong ended
it in one step: the failures were **exactly 1621 iterations apart**, because
a fixed allocation per pass lands the collection at the same point in the
cycle every time. Perfectly periodic, and the period was the clue.

Guarded in layer 05 over ten thousand iterations, many times the old
threshold. `testcases/repro/scan_gc.scm` is kept as the discovery route
and now passes.

#### ✅ Two `fold-i`s in one kernel no longer collide — **done**

A kernel body with two folds used to emit `var acc_2` **twice** at function
scope. With different types the shader is invalid; with the same type it
compiles and the first reader silently gets the second fold's value.

Two causes, and both had to go.

`lib/wgsl.scm` already gensyms an accumulator from the name the `fold-i`
form gives it, so two hand-written folds called `acc` and `tot` always
coexisted. But `wgsl-compile` resets the name counter — deliberately, so
emitted text depends only on the expression and is comparable by string in
the tests — and `wrangle-point-terminal` compiled each of its arguments
through it. The counter therefore restarted per attribute, and two folds
landed on the same digit even when their bases differed. `wgsl-nested`
compiles a sub-expression without the reset, and the terminal uses it; the
property the reset exists for still holds, one level up, per **kernel**
rather than per argument.

And `lib/stage.scm` handed every staged fold the same base name, so two
staged folds collided regardless. A staged accumulator is now named for
what it accumulates — `acc_ys` — which is distinct by construction where
the addresses differ, and reads better besides: the emitted text says
which term the fold belongs to instead of leaving a reader to count folds.

The case that had to work is a **score and a sufficient statistic in one
pass**, which is two reductions over the same address, and which no
renaming scheme can separate on its own. That is why the counter fix was
the necessary half.

Cost, against the estimate: one test assertion, not the renumbering of
every kernel-text comparison this entry used to predict. The other three
such assertions compile directly rather than through a terminal and never
moved.

---

### Planned

#### A gather primitive

Some algorithms need `new[i] = old[a[i]]` — each element taking its value
from an arbitrary other element rather than computing it.

This does **not** break the diagonal write model. `new[i] = old[a[i]]`
still writes only element `i`, so the "return, not clamp" invariant is
untouched. What it breaks is in-place safety: within one dispatch, `i` may
read a slot `j` has already overwritten.

So the copy should be **a primitive, not a wrangle**:

```scheme
(gpu-gather! device dst src indices count)
```

with a fixed shader vxs ships, compiled once, no user WGSL. The wrangle
then stays strictly diagonal permanently, and the double-buffering is an
internal detail of the primitive — a `copyBufferToBuffer` into a cached
temp, then one gather pass. At 1.7 MB the extra copy is nothing.

`indices` should be a **buffer handle, not host bytes**. Filled from the
host through a `shared-layout!` region today; if something on the device
ever computes them instead, it writes the same buffer and the primitive
does not change.

Building the index array on the host is practical rather than a fallback:
a single O(N) pass over 60k elements is ~120k simple VM operations, a few
milliseconds, and nothing needs it every frame.

#### ✅ `define-once` — **done**

A prelude macro — Common Lisp's `defvar` under an honest name. The six
`(if (not (defined? …)) …)` guards in lib/wgsl.scm became `define-once`
forms, the test framework's guard flag (which existed only to guard a
block of counters) dissolved into four self-guarding ones, and
[§5](#define-once) documents the rule for when to reach for it: when the
value's contents come from outside the defining file.

#### The curve-fit demo as a programmable surface — **decided, unstarted**

The shortcut and the real version cost about the same GPU work and differ
in **whether the curve is a parameter of the program**. A hard-coded
quadratic alpha-blended 5000 times is prettier than 40 polylines and says
nothing new. One `curve-elem` next to the model, generating the score
*and* the picture, means swapping in `a·sin(bx) + c` changes the model,
the inference and the plot from one edit — which is a claim you can
demonstrate in ten seconds.

`define-dual` already supplies two of the three consumers. The plot is
the new one.

**It splits into two halves, and only the first carries the thesis.**

- **The visual.** Host inference exactly as it is now (30 ms at K=2000),
  resample N particles as the demo already does with
  `rng-fill-categorical!`, upload their columns, and draw the posterior
  in one **shadertoy** pass that loops over them accumulating near-SDF
  coverage. That is the alpha blend *computed* rather than composited, so
  it needs no new renderer — and at 800×500 with 256 particles and ~20
  flops a contribution it is about 2 Gflop a frame, well under a
  millisecond. `NDRAW = 40` simply becomes 256.
- **The compute.** Move scoring to the device. Needs the staged-name
  binding translation, the `point`-terminal decision and `gpu-gather!` —
  and is **invisible to the thesis**, since "one procedure generates
  model, inference and picture" is equally true with scoring on the host.

So the visual half is reachable without any of the decisions the staged
path is still circling, and the compute half waits for a program that
wants K in the hundreds of thousands.

The one missing piece of infrastructure is small: `lib/shadertoy.scm` has
only a uniform at binding 0, so it cannot read a particle table. It needs
a read-only storage binding with declared accessors — exactly the
`shared-layout!` pattern `lib/wrangle.scm` already implements at binding 3.

#### ✅ Screen-space derivatives, not autodiff, for the near-SDF — **done**

A uniform-width stroke needs the distance to the curve, and the
first-order estimate wants `f′`. It is tempting to reach for autodiff.
Don't: `dpdx`/`dpdy` are better suited, not merely cheaper.

```scheme
;; g is the residual field f(px) - py, in plot coordinates
(/ (abs g) (length (vec2 (dpdx g) (dpdy g))))   ; distance in PIXELS
```

Normalising an implicit function by its screen-space gradient gives the
distance **already in pixel units**. The analytic form
`|g| / sqrt(1 + f′²)` gives *plot* units and still needs converting — and
plot axes almost never have equal scales, so it then wants the slope
rescaled by `sx/sy` before the perpendicular distance means anything on
screen. `dpdy(g)` picks the vertical scale up on its own, so the
anisotropy handles itself.

It also costs the thesis nothing: there is still exactly one definition
of the curve. A hardware derivative **measures** it rather than being a
second definition of it, so there is nothing that can drift.

`dpdx`, `dpdy` and `fwidth` are in the kernel language, and **gated on a
stage**. `wgsl-stage-of` is an exception list, not the beginning of a
stage system: `shadertoy` compiles as `:fragment` and admits them,
`wrangle-scheme` compiles as `:compute` and refuses them, and an
un-staged compile refuses them too — "not saying" must not count as
permission, because that is exactly the case where nothing knows where
the code will run. `with-wgsl-stage` restores the stage even if the
compile raises.

✅ **Uniformity: answered.** `demos/basis.scm` calls `dpdx` and `dpdy`
inside a `fold-i` over the particles, and the shader compiles and draws —
so a fold with a uniform trip count does preserve uniform control flow as
far as a real implementation is concerned. The strokes hold their width
where the curves steepen, which is the screen-space normalisation working
rather than merely compiling.

Still unchecked: **curvature within a quad**, since the estimate is a
one-pixel finite difference and misestimates where `f′` swings hard
across 2×2 pixels. A high-frequency `sin(bx)` is the case to look at,
which is exactly the curve worth swapping in to show the surface off.

#### ⚠️ Infinities and NaN reach a device, but WGSL does not promise it

`-inf`, `inf` and `nan` are literals the reader already understands, and
a kernel may write them — so a model saying *this configuration is
impossible* (the rocket exploded; score it `-inf`) needs no vocabulary it
would not otherwise have. WGSL has no spelling for any of them and will
not compute one at shader-creation time, so the emitter turns each into a
call to a helper that divides by a **runtime** `var`.

The caveat is the language's, not the mechanism's: **WGSL permits an
implementation to assume infinities and NaNs do not arise**, and to yield
an indeterminate value where one would. Hardware f32 produces them, so
this works in practice — but it wants an eye on a real device before a
score depends on it, and that check cannot be a test (see
[fake_webgpu](#fake_webgpujs-counts-dispatches-it-does-not-execute-wgsl)).

One consequence worth thinking about before a model leans on it: if
*every* particle scores `-inf`, `logsumexp` computes `-inf − (−inf)` and
the whole population turns to NaN. A finite floor — the most negative
f32 — degrades into uniform weights instead, which is at least a
survivable state. Whether "impossible" should mean the IEEE value or a
floor is a question the first model to fail will answer better than this
paragraph can.

#### `lib/stat.wgsl`'s `gamma_core` could return a NaN now

Its comment reads *"Argh. creating a NaN, which I would prefer to return,
is nontrivial in wgsl"* — it fabricates `1.0`, a perfectly plausible
gamma value, when the rejection loop gives up. The `var` trick above
makes a NaN available.

**Not done, deliberately.** The host fabricates `1.0` too and counts it
in `dist-failures`, so changing one side alone would break the
correspondence the whole port exists to keep. It is a change to *both*,
or neither.

#### `batch-i` allocates a distribution per element — **noted, not scheduled**

`batch-i*` calls `(f j)` per index, and `(normal <expr> sigma)` builds a
form list, a record and four closures — every element, of every particle.
`batch` builds one distribution and hands a column to a native sum.

Measured, separating the two components (the per-particle fixed cost is
identical, ≈12.3 µs, so the whole difference is per element):

| | per element | at N=10 | at N=80 |
|---|---|---|---|
| `batch` + column + native sum | 0.18 µs | — | — |
| `batch-i` | 1.05 µs | — | — |
| ratio | **5.8×** | 1.6× | 3.6× |

Flat in K — both paths are linear in it, so this never amortises away —
and growing in N toward the per-element ratio as the fixed cost dilutes.

The waste is allocation, not arithmetic: both compute the same
polynomial and the same log-density. The fix, if it is ever wanted, is
that `batch-i` asks for a distribution OBJECT per index when only the
PARAMETERS vary. The macro can see its own body, so a recognised family
could resolve the score function once and evaluate only the parameter
expressions per index, falling back to the general path for anything it
does not recognise. That makes the expansion depend on the shape of the
body, which is why it is written down here rather than done.

It matters more than it looks: if the stageable form becomes the default
way to write a model, this is a tax on the fiber path for models that
never go near a device.

One thing it already tells us. `demos/curvefit.scm` reports
`us-per-particle` as "the number to plan with", and extrapolating it in K
is sound. It is **not** a baseline for the staged path, and not because
of the ratio above: its speed rests on ONE scratch buffer "rewritten in
place for every particle", which is correct only while exactly one
particle exists at a time. A parallel-safe version of that same
vectorisation needs K×N storage — the two-axis column
[§6](#a-gather-primitive) refuses. The optimisation is licensed by
sequentiality, so it cannot cross to the device with the model.

#### ✅ The staged kernel, answered by a real device

§5c's two backends were compared only in f64 until `demos/measure.scm`
ran the emitted kernel. It dispatches the staged log-joint over 512
importance particles, reads the scores back, and compares them against
`staged-logpdf`. Every input is written into an f32 buffer and read back
out before either side sees it, so the two differ in the width of the
arithmetic and in nothing else.

At n = 32 observations, against the f64 oracle:

| | mean error | bias | worst relative |
|---|---|---|---|
| plain fold | 2.59 ulp | −2.01 ulp | 3.6e-7 |
| blocked ×4 | **0.79 ulp** | **+0.12 ulp** | 2.4e-7 |

Sub-ulp and unbiased is the representable floor, so the reading is that
the kernel computes the same function and the residue is f32 itself.
Worst relative sits at about twice f32's epsilon.

**What the bias was, and the trap in finding it.** `with-staged-f32`
narrows the VM to the device's width, and it was written to test whether
the plain fold's −2 ulp bias came from the accumulator. It showed the bias
surviving, which looked like an acquittal and was not: rounding both sides
in the same sequential order makes a shared flaw cancel in the
subtraction. Controlling a variable by making it identical on both sides
hides its size rather than measuring it. The bias was the accumulation all
along, and reassociating the fold removed it.

**Compensated summation does not survive this compiler.** Kahan was
implemented and measured. On the host it changes 311 of the 512 particles
and cuts summation error 3×; on the device it returned results
bit-identical to the plain sum for all 512. `(t - sum) - y` is
algebraically zero and the optimiser deleted it. Removed rather than kept
as dead code that looks alive. The same fate awaits double-single pairs,
which rest on the same two-sum — and there is nothing to widen to, since
WGSL has no f64 and Metal no double. Of the three ways to fix a float sum,
reassociation is the only one available here.

**`with-staged-f32` is kept but is not an oracle.** It predicted blocked
×4 would gain 1.9× where the device gained 3.3× — it mispredicted the
device's own arithmetic by nearly two, in the one quantity it models. So
it stays as the instrument it is, and the standing conclusion is that the
device's arithmetic is the only reliable statement about the device's
arithmetic. Emulating f32 on the host to tighten the comparison would
inherit exactly that error.

Blocked summation is **off by default**: see `lib/stage.scm` on why n = 32
does not need it and why n = 10,000 would.

#### Conjugate structure reads from the IR; the kernel does not exist yet

`lib/gibbs.scm` answers whether a model has a closed-form conditional at
an address, and recovers its coefficients if it does. It reads the staged
IR rather than the model source, because the IR is already normal form,
and the whole reading is static — nothing is witnessed by running the
model.

```scheme
(gibbs-structure st :a)   ; => {:kind normal-normal :prior (m0 s0) :children (...)}
(gibbs-posterior st g ch) ; => (loc . scale)
```

For the curve model the coefficient of `:a` comes back as
`(* (data xs j) (data xs j))` and its offset as
`(+ (* (choice :b) (data xs j)) (choice :c))` — both read out of
`curve-elem`'s **body**, since from the call site the mean is only
`(call curve-elem ...)`. Another choice appearing in the offset is correct
rather than leakage: a Gibbs conditional holds every other address fixed,
so `(choice :b)` there is a conditioned constant.

**A Gibbs update can be checked, and the first attempt at saying otherwise
was wrong.** A second derivation from the same structural claim repeats
the same mistake, so the two-path comparison that serves everything else
here fails — two-path detection assumes the paths fail independently. But
the claim divides. That the conditional is *Gaussian* is a property of the
text, proved by the refusals. Which Gaussian it *is* then follows from the
model and can be measured: a Gaussian conditional makes the log-joint
exactly quadratic in the address, so three evaluations of `staged-logpdf`
recover the mean and scale as an identity — no fit, no residual, no step
size to tune. `gibbs-posterior-by-probe` is that instrument, it touches no
coefficient the derivation produced, and the closed form agrees with it to
1e-11 with the result independent of the step over a 300x range.

**Checked against an independent implementation, and bit-identical.** The
same pair has a hand-written update in a Warp-lineage compiler this design
takes its bearings from, and that compiler builds the update the other way
round: the coefficient from an explicit design function, the residual from
an explicit loop over the other coefficients. Transcribed to f64 and run
on the same data, loc and scale agree with `gibbs-posterior` for all three
coefficients at **zero difference**. Two derivations, two languages, one
formula — and the affine decomposition was written and passing before the
reference was read, so the agreement is a check rather than a copy.

Refused by name: a mean quadratic in the address, the address in a child's
scale or in a divisor, an address flowing through a DECLARED-only helper
(no body in this language, so affineness cannot be read — the message says
`define-dual`), an unknown address, a batched choice. A DECLARED helper
applied to *data* does not block anything, because an expression free of
the address is a constant for this update whatever it contains.

What is missing is the whole device half, and two things gate it. The
subset has **no gather**, so `mu[z]` cannot be expressed and every
conjugacy that needs an assignment — a mixture's mean, a Dirichlet over
categories — is out of reach until [§6](#a-gather-primitive) lands. The
other gate is gone: a Gibbs kernel wants a score and a sufficient
statistic in one pass, and two reductions in one kernel now coexist. So
this is the fiber path only, for one reason rather than two.

### Infrastructure

#### ✅ A staleness guard for `web/vxs.wasm` — **retired, premise gone**

This entry proposed a test against a committed `.wasm` drifting from its
source. The artifact is no longer checked in (`.gitignore` has both
`web/vxs.js` and `web/vxs.wasm`; `make -C src wasm` builds them, and
emscripten's absence is reported plainly rather than as a missing-file
error). `make test` depends on the `wasm` target, so make's own
dependency tracking is the guard this entry asked for — a stale build
cannot survive a test run.

The residual staleness risk is the browser cache serving an old build,
which is what the BUILDSTAMP baked into the binary exists to expose:
compare the stamp the page prints against the build you just made.

#### `fake_webgpu.js` counts dispatches; it does not execute WGSL

`dispatchWorkgroups()` increments a counter. So everything up to a
dispatch is testable headlessly — the emitted kernel type-checks, the
bindings resolve, the host backend agrees with `assess` — and **numbers
coming back off a device are not**. A device-versus-host comparison needs
a browser and a look.

This is what pushed the staged compiler to lower to an **IR with two
backends** rather than straight to WGSL, and that turned out better than
the thing it replaced: it separates whether the compiler *read* the model
correctly (structural, exactly reproducible, and now asserted
bit-identical against `assess`) from whether f32 agrees with f64
(numerical, already characterised in [§5a](#why-a-port-and-not-a-better-design)).
Testing them together at one tolerance would let a structural bug hide
under it.

Worth knowing before starting any device-numeric work: that comparison is
the first thing in this project that cannot be machine-checked, so it
wants an eye rather than a test.

⚠️ It does not COMPILE WGSL either, and that is a wider gap than the
numbers. `createShaderModule` keeps the text and returns whatever
`compileMessages` says about it; the preset suite passes `() => []`, so
every shader compiles clean by construction and any module the browser
would reject passes in node. A leak across presets was found in the
browser and the harness could not see it in either state — with the bug
in or the bug fixed, all ten pairs reported clean. **A shader-text
assertion in Scheme is the check that works**; a green preset run is not
evidence that a shader compiles. The `compileMessages` hook is the place
to put a cheap scan if one is ever wanted — it receives the code.

#### ✅ A definition outlived the layout it called

`define-gpu` registers into a table that outlives the program that wrote
it, and every wrangle module splices all of it. `demos/measure.scm`
bridges two naming conventions with two shims — `(define-gpu (xs (j
:u32)) (shared-xs j))` — and a shared accessor lives only as long as the
layout that declared it. So after viewing `measure`, the next preset to
call `shared-layout!` retracted `shared_xs` and still emitted `fn xs`
calling it: `unresolved call target 'shared_xs'`.

`shared-layout!` already retracted the stale *declarations*, and the
comment above it explains why. This was the same hole from the other
side — a definition promising nothing rather than a declaration promised
nothing — so the fix is the symmetric half: retract the definitions whose
bodies call a retracted accessor. Precise rather than blanket, and that
matters, because the table also holds library definitions registered at
load time which no layout switch may touch (layer 18 asserts
`logpdf-normal` survives). `wgsl-forget-definitions!` was already there,
never called, and could not have been used: it cannot tell the two kinds
apart.

One level deep, which is every shim that exists. A `define-gpu` calling
a shim rather than an accessor would survive and want a fixpoint.

#### Migrate the classic testcases into the ground-up suite

Not urgent. The 13 `vx-test.scm` cases move
from I/O-diff-against-golden-file to `assert-equal`-on-return-value —
except `r4rstest.scm`, which is fundamentally a printing conformance suite
and has to stay on the I/O path. Keep `dynamic.scm` whatever happens: its
value is the stress profile (self-parsing 2300 lines), not the answer.

---

### Parked ideas

Not scheduled, kept so they are not rediscovered from scratch.

- **Differentiating the IR, when a posterior asks.** Autodiff's payoff is
  MALA or short-trajectory HMC — the difference between a random-walk
  kernel that mixes badly past a couple of dimensions and one that does
  not. Rendering does not need it (see the near-SDF entry above).

  The target is **not** `curve-elem`. It is the staged IR, differentiated
  with respect to a named choice, which is a structural recursion over a
  small closed grammar — `number`, `choice`, `choice-i`, `data`, `call`,
  the four arithmetic ops, `sum-over`, `scan-over` — with `call` reaching
  into a dual's source, since a dual's body is a datum too.

  **Wait for the trigger, which is a posterior that demonstrably will not
  mix under a random walk.** That moment says what the gradient has to
  handle, and the answers are not guessable in advance: how many
  dimensions; whether `scan-over` must be differentiated *through* (the
  adjoint of a trajectory, a much larger thing than an expression);
  whether `flip` needs a reparameterisation story at all. The pendulum's
  parameter posterior is smooth and low-dimensional, so a random walk
  will be fine there — it is the double pendulum, or anything with a
  banana, that will ask.

- **Per-cube orientation** in the cubes renderer.
- **Overcooked-shaped actors** — goal-directed agents for the outside demo,
  as opposed to the planning/goal work that stays inside the org.
- **`gpu.html`'s "Scheme it runs" pane** still shows the triangle program
  regardless of which demo is running.
- **`define-record-type`** — parked when WebGPU work took priority.
- **`OP_LOOP`** — named-let entry costs one closure plus one box per
  captured variable, per entry. The "thread, don't capture" idiom works
  around it; an opcode would remove it.
- **Geodesics on a torus.** A good fit for a reason that is not aesthetic:
  a geodesic is an ODE, so where a particle is at time *t* is a function of
  everywhere it has been, not of *t*. A shader structurally cannot do it,
  and per-element scratch attributes are exactly the machinery it needs —
  which makes it the sharpest available demonstration of what the compute
  side is for.

  It also comes with a **conserved quantity**, so correctness is
  measurable rather than eyeballed. Clairaut's relation makes
  `r(v)·sin ψ` invariant along a geodesic, with `r(v) = R + a·cos v`;
  integrate a few thousand steps and assert it holds. That is the same
  move as asserting gradient noise is exactly zero at lattice points.

  The invariant also predicts the interesting behaviour instead of
  discovering it: small angular momentum confines a geodesic to the outer
  region, because `r(v)` has a minimum it cannot cross. Two populations —
  some winding freely, some trapped — from one conserved number. The
  self-intersecting regime is where `a > R` and that minimum stops
  existing.

- **Runge-Kutta on the GPU: the tableau folds at COMPILE time.** An RK step
  is a fold over the rows of a Butcher tableau, but `fold-i` cannot carry
  seven stage-vectors as an accumulator and does not need to — the tableau
  is known when the kernel is generated, so Scheme does the fold and emits
  UNROLLED WGSL. Zero coefficients then emit nothing at all, which is an
  optimisation you would otherwise get around to and here is just what code
  generation produces.

  ⚠️ **Adaptive step size is the one part that does not port.** Different
  elements would take different numbers of steps, and a warp runs until its
  slowest lane finishes, so per-element step control costs the whole warp
  its divergence. The division that follows: **fixed step on the device,
  with the CPU oracle establishing what step size the accuracy target
  entitles you to.**

- **An adaptive ODE solver as a coroutine, for use as an ORACLE.** A
  high-order extrapolation solver with dense output has a control-flow
  shape that keeps being written inside out: a callback invoked once per
  accepted step, handed a closure valid over that step's interval. That is
  `yield`, pushed rather than pulled.

  The suspension lands at a tractable boundary. One accepted step runs
  uninterrupted in native code and returns a dense segment, so nothing has
  to suspend *inside* the integrator — only the outer driver loop needs
  inverting, and C++20 `co_yield` does that without hand-rolling a state
  machine or touching the numerics. Two coroutine layers, one C++ and one
  Scheme, meeting at a handle.

  **The role is oracle, not workhorse.** A derivative written in Scheme is
  correct — it is pure arithmetic and never needs to suspend — but an
  order-12 step spends hundreds of evaluations, and this VM is roughly
  three orders of magnitude off V8 on tight arithmetic. So: many cheap
  trajectories in a wrangle for the picture, a few at 1e-12 for the truth,
  and then the cheap integrator's error becomes *measurable* rather than
  assumed.

  **The target should be DOPRI5 rather than an extrapolation method.** It
  is one tableau and one interpolation polynomial; it carries dense output;
  and it is strong enough for this class of problem — two Arenstorf orbits
  at 1e-7 in 453 accepted steps. An extrapolation method is more powerful
  and much larger, and transliterating careful numerical code twice is
  where the care leaks out of it.

  **Dense output is what makes that comparison possible at all.** The two
  integrators take unrelated step sizes, so there are no matched samples to
  compare; dense output evaluates the reference at exactly the times the
  cheap one produced. This project has an RNG oracle (published
  known-answer vectors) and would have a geometric invariant (Clairaut). It
  has nothing trustworthy for trajectories, and that is the gap.

- **Symbolic differentiation of the kernel language.** The WGSL compiler is
  a pure-expression compiler over a small closed set of forms, which is
  precisely the setting where symbolic differentiation is tractable —
  `d/dx` of an expression is another expression in the same language. That
  would turn a scalar field written once into its own gradient, with no
  finite differences and no second version to keep in step. Related to the
  geodesic entry above: equations of motion derived rather than
  transcribed.

- **A fibration mixer.** Stage 1 of the ensemble demo passes through
  configurations resembling a Hopf fibration, because each actor traces a
  closed curve on a torus and 96 of them at different radii foliate nested
  tori. The resemblance is structural rather than coincidental, though
  Hopf circles are genuinely linked and these merely share the tori.

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
- `shared` is a **reserved word** in WGSL. The shared binding is spelled
  `sdata`; a binding named `shared` will not compile.
- `let` in the kernel language binds **sequentially**, unlike R7RS `let`.
  `let*` is accepted as the same form.
- A `fold-i` index is `:u32`. Using it as a number needs `(f32 k)`, because
  WGSL has no implicit coercion.
- A conditional draw is not conditional: `if` is a selection, so a
  `random-*` in either arm advances the stream every time. §4.
