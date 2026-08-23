//----------------------------------------------------------------------
// Gradient (Perlin) noise, addressed by lattice coordinate
//
// Perlin noise needs a pseudo-random gradient at every integer lattice
// point, and the classical way to get one is a permutation table or a
// hand-rolled integer hash. Both are things you invent, and neither is
// checkable — a bad hash shows up as visible lattice structure, which is
// exactly the kind of "looks a bit wrong" that costs an afternoon.
//
// A counter-based RNG is addressed BY INDEX. A lattice point IS an index.
// So the gradient at (ix, iy, iz) is one Threefry block with those three
// integers as the counter — no table, no hash, and the generator underneath
// is the one checked against published known-answer vectors.
//
// It also touches NO private state. rng_init and friends keep a per-
// invocation stream in module-scope `var<private>`, so a noise call that
// went through them would silently consume a kernel's draws and shift
// every random decision downstream of it. One block, computed and
// discarded, cannot.
//
// Requires: lib/rng.wgsl (threefry4x32).
//----------------------------------------------------------------------

// A u32 to [0,1), by the same construction rng_unit uses: paste the top 23
// bits into a float's mantissa with an exponent of 0, giving [1,2), then
// subtract. Exact, and no division.
fn noise_unit(x: u32) -> f32 {
  return bitcast<f32>((x >> 9u) | 1065353216u) - 1.0;
}

// The gradient at one lattice point: a unit vector, uniform on the sphere.
//
// Uniform means z uniform on [-1,1] and the angle uniform on [0,2pi) —
// Archimedes' theorem. Taking two angles instead would crowd the poles and
// give the noise a visible axis.
fn noise_grad(ix: i32, iy: i32, iz: i32, seed: u32) -> vec3<f32> {
  let b = threefry4x32(vec4u(bitcast<u32>(ix), bitcast<u32>(iy), bitcast<u32>(iz), 0u),
                       vec4u(seed, 0u, 0u, 0u));
  let z = 2.0 * noise_unit(b.x) - 1.0;
  let a = 6.28318530718 * noise_unit(b.y);
  let r = sqrt(max(0.0, 1.0 - z * z));
  return vec3<f32>(r * cos(a), r * sin(a), z);
}

// Perlin's quintic fade, 6t^5 - 15t^4 + 10t^3. Its first and second
// derivatives vanish at 0 and 1, which is what stops the lattice showing
// up as creases; the cubic smoothstep does not have that property.
fn noise_fade(t: vec3<f32>) -> vec3<f32> {
  return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

// Gradient noise at p. Roughly [-1, 1], though the extremes are rare.
fn perlin3(p: vec3<f32>, seed: u32) -> f32 {
  let pi = floor(p);
  let pf = p - pi;
  let u = noise_fade(pf);
  let ix = i32(pi.x);
  let iy = i32(pi.y);
  let iz = i32(pi.z);

  var acc = 0.0;
  for (var c = 0u; c < 8u; c = c + 1u) {
    let dx = i32(c & 1u);
    let dy = i32((c >> 1u) & 1u);
    let dz = i32((c >> 2u) & 1u);
    let g = noise_grad(ix + dx, iy + dy, iz + dz, seed);
    let d = pf - vec3<f32>(f32(dx), f32(dy), f32(dz));
    // Trilinear weight, multiplied out rather than nested mixes: each axis
    // contributes u or 1-u according to which side of the cell we are on.
    let wx = mix(1.0 - u.x, u.x, f32(dx));
    let wy = mix(1.0 - u.y, u.y, f32(dy));
    let wz = mix(1.0 - u.z, u.z, f32(dz));
    acc = acc + wx * wy * wz * dot(g, d);
  }
  return acc;
}

// Three independent fields, as one vector. Adjacent seeds are fine: they
// are Threefry KEYS, and the whole point of the cipher is that neighbouring
// keys produce unrelated streams.
fn perlin3v(p: vec3<f32>, seed: u32) -> vec3<f32> {
  return vec3<f32>(perlin3(p, seed),
                   perlin3(p, seed + 1u),
                   perlin3(p, seed + 2u));
}

// Fractal sum: octaves at doubling frequency and halving amplitude.
// Normalised so the result keeps roughly the range of a single octave.
fn fbm3(p: vec3<f32>, seed: u32, octaves: u32) -> f32 {
  var acc = 0.0;
  var amp = 1.0;
  var norm = 0.0;
  var q = p;
  for (var o = 0u; o < octaves; o = o + 1u) {
    acc = acc + amp * perlin3(q, seed + o * 8u);
    norm = norm + amp;
    amp = amp * 0.5;
    q = q * 2.0;
  }
  return acc / max(norm, 1e-6);
}
