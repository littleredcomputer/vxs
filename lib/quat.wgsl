//----------------------------------------------------------------------
// Quaternions
//
// Orientation is stored as a quaternion — four floats, (x, y, z, w) with
// the scalar LAST, which is what makes q.xyz and q.w read cleanly below
// and matches the posquat convention (px py pz qx qy qz qw).
//
// Why not a matrix: sixteen floats against four, read per-vertex from a
// storage buffer where bandwidth is the actual cost. A matrix also admits
// shear and scale that nothing here wants, and drifts under repeated
// composition in ways a quaternion does not. It is marginally cheaper to
// apply — about 15 flops against 20 — and that is the least important
// number in the comparison.
//
// Why not store the rotation vector directly: because of WHERE the work
// lands. A cube's rotation is applied 36 times, once per vertex, and each
// invocation is independent, so nothing amortises. Storing a rotation
// vector puts a sin, a cos and a normalize in all 36. Storing a quaternion
// puts them in NEITHER — q_rot is two cross products and no transcendental
// at all — and leaves exactly one conversion, in the kernel, once per
// element.
//
// The rotation vector is still the right form to GENERATE from, which is
// what q_from_rotvec is for. It is singularity-free at zero: precisely
// where the axis stops being meaningful, the angle vanishes. Building an
// orientation by aiming an axis at a direction cannot manage that — there
// is no continuous way to choose the remaining roll (hairy ball theorem),
// so it snaps somewhere, and on a slowly moving field the snap is what the
// eye finds.
//----------------------------------------------------------------------

fn q_identity() -> vec4<f32> {
  return vec4<f32>(0.0, 0.0, 0.0, 1.0);
}

// Rotate v by q. Two crosses, no trigonometry — this is the one that runs
// per vertex, so it is the one that had to be cheap.
fn q_rot(q : vec4<f32>, v : vec3<f32>) -> vec3<f32> {
  let t = 2.0 * cross(q.xyz, v);
  return v + q.w * t + cross(q.xyz, t);
}

// A rotation of |rv| radians about rv. Continuous at rv = 0, where it
// returns the identity rather than dividing by nothing.
fn q_from_rotvec(rv : vec3<f32>) -> vec4<f32> {
  let t = length(rv);
  if (t < 1e-8) { return q_identity(); }
  let h = t * 0.5;
  return vec4<f32>(rv * (sin(h) / t), cos(h));
}

fn q_from_axis_angle(axis : vec3<f32>, angle : f32) -> vec4<f32> {
  let h = angle * 0.5;
  return vec4<f32>(normalize(axis) * sin(h), cos(h));
}

// Hamilton product: the rotation b followed by the rotation a.
fn q_mul(a : vec4<f32>, b : vec4<f32>) -> vec4<f32> {
  return vec4<f32>(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz),
                   a.w * b.w - dot(a.xyz, b.xyz));
}

fn q_conj(q : vec4<f32>) -> vec4<f32> {
  return vec4<f32>(-q.xyz, q.w);
}

// Only needed if a program INTEGRATES orientation over time; a quaternion
// recomputed from a field each frame cannot drift.
fn q_normalize(q : vec4<f32>) -> vec4<f32> {
  let n = length(q);
  if (n < 1e-8) { return q_identity(); }
  return q / n;
}
