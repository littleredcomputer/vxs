// Threefry-4x32-13 on the GPU — the same generator as lib/threefry.scm.
//
// This replaces the pcg3d stream that lib/stat.wgsl was originally written
// against. The reason is not that pcg3d is bad; it is that a STREAM is the
// wrong shape here. A stream makes each draw depend on how many draws came
// before it, so a point's randomness depends on evaluation order — useless
// when thousands of invocations run in an unspecified order, and impossible
// to reproduce on the host.
//
// Threefry is a pure function of (counter, key). Address the counter by
// point number and every point gets its own independent, reproducible
// randomness with no coordination at all. And because it is bit-for-bit the
// same algorithm the host runs, a value drawn here can be checked against
// one drawn in Scheme — the nine published known-answer vectors in
// testcases/suite/13_threefry.scm hold for this code too.
//
// A block is four words. Drawing one word at a time and refilling every
// fourth keeps the cost at 13 rounds per FOUR draws rather than per draw.

fn rotl32(x: u32, n: u32) -> u32 {
  // Safe only because every rotation constant below is in 5..27. A rotate
  // by 0 or 32 would shift by the full width, which WGSL leaves undefined.
  return (x << n) | (x >> (32u - n));
}

fn threefry4x32(ctr: vec4u, key: vec4u) -> vec4u {
  var rot = array<u32, 16>(
    10u, 26u, 11u, 21u, 13u, 27u, 23u,  5u,
     6u, 20u, 17u, 11u, 25u, 10u, 18u, 20u);
  let parity = 0x1BD11BDAu;   // Skein key-schedule parity
  var ks = array<u32, 5>(
    key.x, key.y, key.z, key.w,
    parity ^ key.x ^ key.y ^ key.z ^ key.w);

  var x = vec4u(ctr.x + ks[0], ctr.y + ks[1], ctr.z + ks[2], ctr.w + ks[3]);

  for (var r: u32 = 0u; r < 13u; r = r + 1u) {
    // Key injection at the TOP of every fourth round. 13 is not a multiple
    // of four, so no trailing injection is needed after the loop; a
    // different round count would need one, which is why 13 is baked in
    // here rather than being a parameter.
    if (r > 0u && (r % 4u) == 0u) {
      let s = r / 4u;
      x.x = x.x + ks[(s + 0u) % 5u];
      x.y = x.y + ks[(s + 1u) % 5u];
      x.z = x.z + ks[(s + 2u) % 5u];
      x.w = x.w + ks[(s + 3u) % 5u] + s;
    }
    let i = 2u * (r % 8u);
    let p = rot[i];
    let q = rot[i + 1u];
    if ((r % 2u) == 0u) {
      x.x = x.x + x.y; x.y = rotl32(x.y, p) ^ x.x;
      x.z = x.z + x.w; x.w = rotl32(x.w, q) ^ x.z;
    } else {
      x.x = x.x + x.w; x.w = rotl32(x.w, p) ^ x.x;
      x.z = x.z + x.y; x.y = rotl32(x.y, q) ^ x.z;
    }
  }
  return x;
}

// Per-invocation draw state. Private, so every invocation has its own —
// there is no shared stream and therefore nothing to race over.
var<private> rng_ctr: vec4u;
var<private> rng_key: vec4u;
var<private> rng_block: vec4u;
var<private> rng_avail: u32 = 0u;

// Call once per invocation before drawing. `ptnum` separates points from
// each other; `seed` separates one run from another; `stream` separates
// independent uses within a single point (positions from colours, say)
// without either of them disturbing the other's sequence.
fn rng_init(ptnum: u32, seed: u32, stream: u32) {
  rng_ctr = vec4u(ptnum, 0u, stream, 0u);
  rng_key = vec4u(seed, 0u, 0u, 0u);
  rng_avail = 0u;
}

fn rng_u32() -> u32 {
  if (rng_avail == 0u) {
    rng_block = threefry4x32(rng_ctr, rng_key);
    rng_ctr.y = rng_ctr.y + 1u;
    rng_avail = 4u;
  }
  let v = rng_block.x;
  rng_block = vec4u(rng_block.y, rng_block.z, rng_block.w, 0u);
  rng_avail = rng_avail - 1u;
  return v;
}

// A float in (0,1) from the top 23 bits: build an f32 in [1,2) by pasting
// the mantissa under a fixed exponent, then subtract one. Exact, and it
// avoids a division.
//
// NEVER EXACTLY ZERO, and the clamp is load bearing. The construction
// yields 0.0 for the 512 smallest words out of 2^32 — probability 2^-23,
// which sounds negligible and is about ten draws a second at 86 million
// draws a second. It matters because of what sits downstream:
//
//   random_uniform(-1, 1) then returns exactly -1
//   inv_erf(-1) is inv_erfc(2), whose pp is 0
//   log(0) is -inf, so t is inf
//   the rational term is inf/inf, which is NaN
//   random_normal returns NaN
//
// An element that goes NaN is poisoned permanently and simply vanishes
// from the render, with no diagnostic at all. A kernel with a rejection
// step happens to survive it, because comparisons against NaN are false —
// but that is luck, and it also leans on IEEE semantics a backend built
// with fast-math relaxations is permitted to break.
//
// 2^-24 is half the smallest step this construction can otherwise
// produce, so the clamp removes the endpoint without colliding with any
// real output or perturbing any other value.
fn rng_unit() -> f32 {
  return max(bitcast<f32>((rng_u32() >> 9u) | 1065353216u) - 1.0, 5.9604645e-8);
}
