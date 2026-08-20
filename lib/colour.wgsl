// Colour ramps for point kernels.
//
// Display helpers, deliberately NOT part of the attribute vocabulary — a
// wrangle computes what a point IS, and these decide how to show it. Kept
// in their own file because a ramp is the thing you tweak most, and watch
// mode reloads it on save.

// Cool to hot: dim violet, magenta, red-orange, amber, white.
//
// Piecewise-linear through five control points rather than a formula, so
// the ramp can be art-directed one colour at a time instead of by solving
// for coefficients. The colours themselves are the tuning surface.
//
// The first version of this ran only from dim red to white, and the result
// was a cloud that was uniformly too warm: with no cool end there was
// nowhere for the outskirts to go, and every point landed somewhere in
// red-through-yellow. The violet tail is what gives the fringe somewhere
// to be. It is also why this reads as temperature at all — a gradient
// needs two ends.
//
// The coolest colour is dim but never black. Under additive blending a
// black point is invisible, so the outskirts would not fade, they would
// vanish and take the cloud's silhouette with them.
fn heat_colour(t : f32) -> vec3<f32> {
  let c0 = vec3<f32>(0.08, 0.04, 0.22);   // dim violet
  let c1 = vec3<f32>(0.48, 0.06, 0.32);   // magenta
  let c2 = vec3<f32>(0.87, 0.24, 0.10);   // red-orange
  let c3 = vec3<f32>(0.99, 0.68, 0.15);   // amber
  let c4 = vec3<f32>(1.00, 0.98, 0.88);   // white-hot

  let s = clamp(t, 0.0, 1.0) * 4.0;
  // min against 3 matters at exactly t = 1: without it the last segment
  // degenerates to its own lower endpoint and the hottest points come out
  // amber rather than white.
  let i = min(floor(s), 3.0);
  let f = s - i;

  var a = c0;
  var b = c1;
  if (i >= 3.0)      { a = c3; b = c4; }
  else if (i >= 2.0) { a = c2; b = c3; }
  else if (i >= 1.0) { a = c1; b = c2; }
  return mix(a, b, f);
}

// The same shape run cold: deep blue through teal to white. For when the
// quantity being shown is not energy and warm colours would be a lie
// about the picture.
fn cool_colour(t : f32) -> vec3<f32> {
  let c0 = vec3<f32>(0.04, 0.06, 0.20);
  let c1 = vec3<f32>(0.09, 0.24, 0.55);
  let c2 = vec3<f32>(0.10, 0.55, 0.72);
  let c3 = vec3<f32>(0.35, 0.85, 0.85);
  let c4 = vec3<f32>(0.92, 0.99, 1.00);

  let s = clamp(t, 0.0, 1.0) * 4.0;
  let i = min(floor(s), 3.0);
  let f = s - i;

  var a = c0;
  var b = c1;
  if (i >= 3.0)      { a = c3; b = c4; }
  else if (i >= 2.0) { a = c2; b = c3; }
  else if (i >= 1.0) { a = c1; b = c2; }
  return mix(a, b, f);
}
