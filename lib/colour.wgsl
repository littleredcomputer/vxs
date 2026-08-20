// Colour ramps for point kernels.
//
// Display helpers, deliberately NOT part of the attribute vocabulary — a
// wrangle computes what a point IS, and these decide how to show it. Kept
// in their own file because a ramp is the thing you tweak most and watch
// mode reloads it on save.

// Blackbody-ish, cool to hot: near-black through deep red, orange and
// yellow to white. The channels turn on in sequence rather than being
// interpolated between keyframes — three clamps, no branches, and the
// staggered thresholds are what give it the characteristic bend through
// orange instead of a straight line from red to white.
//
// The cool end is dim rather than black on purpose. Under additive
// blending a black point is invisible, so the outskirts would not fade,
// they would simply vanish and take the cloud's silhouette with them.
fn heat_colour(t : f32) -> vec3<f32> {
  let u = clamp(t, 0.0, 1.0);
  let r = clamp(u * 2.2 + 0.10, 0.0, 1.0);
  let g = clamp(u * 2.2 - 0.75, 0.0, 1.0);
  let b = clamp(u * 2.6 - 1.62, 0.0, 1.0);
  return vec3<f32>(r, g, b);
}

// The same ramp run cold: deep blue through cyan to white. For when the
// quantity being shown is not energy and pretending otherwise would be a
// lie about the picture.
fn cool_colour(t : f32) -> vec3<f32> {
  let u = clamp(t, 0.0, 1.0);
  let b = clamp(u * 2.2 + 0.10, 0.0, 1.0);
  let g = clamp(u * 2.2 - 0.75, 0.0, 1.0);
  let r = clamp(u * 2.6 - 1.62, 0.0, 1.0);
  return vec3<f32>(r, g, b);
}
