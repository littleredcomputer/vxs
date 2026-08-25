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

## 6. Known gaps and open work

What is wrong, what is missing, and what was decided about each. Kept here
rather than in a separate file so that a rule and its known exceptions stay
next to each other.

Nothing here is scheduled. **Decided** marks a course that has been
settled; the rest are candidates.


### Correctness gaps

#### `guard` cannot catch native VM errors — **decided: worth fixing**

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

**Why the cost is acceptable.** Throwing is dearer than setting a field,
but this exception path is not performance sensitive, and no exception here
has a control-flow application the way something like `StopIteration`
would. `ContinuationEscape` (escape-only `call/cc`) is the one exception
that *is* control flow — but it is a single throw site, and a program that
put one in a hot loop would need redesigning rather than a cheaper throw.
So the exception path stays outside the performance envelope by
construction. `RaiseEscape`, the one this change multiplies, has no
control-flow use at all.

#### The reader has no `#x` / `#b` / `#o` literals

`(string->number "FF" 16)` works; `#xFF` is read as a symbol and fails as
an unbound variable. Small, self-contained, and the error message points
nowhere useful.

#### Arithmetic does not type-check

Symptom in [§3](#3-errors-what-is-catchable): non-numbers are treated as
zero, so a typo'd name yields a plausible wrong number rather than a
complaint.

Entangled with keeping `+` first-class and un-opcoded for speed, which is a
deliberate trade — so this is **undecided**. Any fix has to answer what it
costs on the benchmark suite before it is worth having.

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

#### `define-once`

Nearly free; `defined?` exists.

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

Not urgent. The 13 `vx-test.scm` cases move
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
