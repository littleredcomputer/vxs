// Statistical primitives for GPU kernels.
//
// Colin's WGSL stat library, welded onto Threefry. The distributions and
// their log-densities are unchanged; only the source of randomness moved.
// Prepend lib/rng.wgsl before this file — random_uniform is the single
// point where draws enter, and every other sampler is built on it.
//
// The logpdf_* functions are the interesting half for where this is
// heading: a sampler alone gives you particles, but a sampler PLUS its
// log-density gives you importance weights, and that is inference rather
// than decoration.
//
// `fail` counts gamma rejections that gave up after three tries. WGSL has
// no cheap NaN to return, so the count is the only signal that a sample
// was fabricated; a kernel that cares should surface it.
var<private> fail: u32 = 0u;

// From Press NR 3ed.
// A lower-order Chebyshev approximation produces a very concise routine, though with only about single precision accuracy:
// Returns the complementary error function with fractional error everywhere less than 1.2e-7.
fn erfc(x: f32) -> f32 {
  var z = abs(x);
  var t=2./(2.+z);
  var ans=t*exp(-z*z-1.26551223+t*(1.00002368+t*(0.37409196+t*(0.09678418+
    t*(-0.18628806+t*(0.27886807+t*(-1.13520398+t*(1.48851587+
    t*(-0.82215223+t*0.17087277)))))))));
  return select(2.0 - ans, ans, x >= 0.0);
}

// The following two functions are from
// http://www.mimirgames.com/articles/programming/approximations-of-the-inverse-error-function/
fn inv_erfc(x: f32) -> f32 {
  let pp: f32 = select(2.0 - x, x, x < 1.0);
  let t: f32 = sqrt(-2.0 * log(pp/2.0));
  var r: f32;
  var er: f32;

  r = -0.70711 * ((2.30753 + t * 0.27061)/(1.0 + t * (0.99229 + t * 0.04481)) - t);
  er = erfc(r) - pp;
  r += er/(1.12837916709551257 * exp(-r * r) - r * er);
  //Comment the next two lines if you only wish to do a single refinement
  //err = erfc(r) - pp;
  //r += err/(1.12837916709551257 * exp(-r * r) - r * er);
  r = select(r, -r, x>1.0);
  return r;
}

fn inv_erf(x: f32) -> f32 {
  return inv_erfc(1.0-x);
}

fn random_normal(loc: f32, scale: f32) -> f32 {
  let u = sqrt(2.0) * inv_erf(random_uniform(-1.0, 1.0));
  return loc + scale * u;
}

// De-compiled from JAX genjax.normal.logpdf
fn logpdf_normal(v: f32, loc: f32, scale: f32) -> f32 {
  let d = v / scale;
  let e = loc / scale;
  let f = d - e;
  let g = pow(f, 2.0);
  let h = -0.5 * g;
  let i = log(scale);
  let k = 0.9189385175704956 + i;
  return h - k;
}
// recovered from de-compiled JAX.
//
// The ONLY function that draws randomness: everything else in this file is
// built on it, so rewiring the generator is a change to these two lines and
// nothing else. It used to advance a private pcg3d seed; it now takes a
// word from the Threefry block in lib/rng.wgsl, which is addressed by point
// number rather than by position in a stream.
fn random_uniform(low: f32, high: f32) -> f32 {
  let a: f32 = rng_unit();
  let diff = high - low;
  let w = diff * a;
  let u = w + low;
  return max(low, u);
}

fn random_flip(prob: f32) -> bool {
  return random_uniform(0.0, 1.0) < prob;
}

// recovered from de-compiled JAX
fn logpdf_flip(v: f32, p: f32) -> f32 {
  let g = -p;
  let h = log(g + 1.0);  // log1p
  let i = log(p);
  let k = 1.0 - v;
  let l = k == 0.0;
  let n = h * k;
  let o = select(n, 0.0, l);
  let q = i == 0.0;
  let r = i * v;
  let s = select(r, 0.0, q);
  return o + s;
}

// recovered from de-compiled JAX
fn logpdf_uniform(v: f32, low: f32, high: f32) -> f32 {
  let d = v != v;
  let e = v < low;
  let f = v > high;
  // g = e, h = f
  let i = e || f;
  let j = high - low;
  let k = 1.0 / j;
  let l = select(k, 0.0, i);
  let q = select(l, v, d);
  return log(q);
}

fn random_exponential(lambda: f32) -> f32 {
  let u = 1.0 - random_uniform(0.0, 1.0);  // u is in (0, 1]
  return - log(u) / lambda;
}

// The squeeze itself, valid only for alpha >= 1. Below that d = alpha-1/3
// goes non-positive, sqrt(9d) is the root of a non-positive number, the
// acceptance test can never pass, and a fabricated 1.0 comes back — a
// perfectly plausible gamma value. Measured on the host before the boost
// below existed: alpha <= 1/3 ALWAYS fabricated, and alpha = 0.5
// fabricated 8 times in 2000.
// https://dl.acm.org/doi/pdf/10.1145/358407.358414
fn gamma_core(alpha: f32) -> f32 {
  let d = alpha - 1.0/3.0;
  for (var i: u32 = 0u; i < 3u; i++) {
    let x = random_normal(0.0, 1.0);
    let u = random_uniform(0.0, 1.0);
    let v = pow(1.0 + x/sqrt(9.0 * d), 3);
    let dv = d * v;
    if (log(u) < 0.5 * pow(x,2) + d - dv + d * log(v)) {
      return dv;
    }
  }
  fail++;
  return 1.0; // Argh. creating a NaN, which I would prefer to return, is nontrivial in wgsl
}

// Marsaglia and Tsang's own remedy for alpha < 1, from the same paper:
//
//   Gamma(alpha) = Gamma(alpha + 1) * U^(1/alpha)
//
// It extends validity to every alpha > 0 and keeps the squeeze in the
// regime it is good at, so the fabrication rate falls rather than merely
// stopping at zero — measured on the host at 4 in 40000, from 8 in 2000.
//
// CONSUMPTION ORDER: the boost uniform is drawn AFTER the core's draws.
// That is part of the contract, not an implementation detail — the host
// consumes in this order and the two agree only while both do.
fn random_gamma_theta_one(alpha: f32) -> f32 {
  if (alpha < 1.0) {
    let g = gamma_core(alpha + 1.0);
    let u = random_uniform(0.0, 1.0);
    return g * pow(u, 1.0/alpha);
  }
  return gamma_core(alpha);
}

fn random_gamma(alpha: f32, lambda: f32) -> f32 {
  let theta = 1.0 / lambda;
  return theta * random_gamma_theta_one(alpha);
}

const ln_2pi = log(2.0*pi);
const pi = radians(180.0);

fn g2d_logpdf(g: mat3x3f, x: vec2f) -> f32 {
  // the gaussian g is a packed representation of a 2D multinormal
  // https://en.wikipedia.org/wiki/Multivariate_normal_distribution
  //   PDF(k=2): exp( -0.5 * (x-µ)' * inv(Σ) * (x-µ) ) / (sqrt(det(Σ)) * 2 * π)
  // therefore:
  //   LogPDF:   -0.5 * ((x-µ)' * inv(Σ) * (x-µ)) - ln(2π) - 0.5 * det(Σ)
  let mu = (g[2]).xy;
  let sigma = mat2x2f(g[0].xy, g[1].xy);
  let det = sigma[0][0] * sigma[1][1] - sigma[1][0] * sigma[0][1];
  let invDet = 1.0/det;
  let invSigma = invDet * mat2x2f(sigma[1][1], -sigma[0][1], -sigma[1][0], sigma[0][0]);
  let a = dot(x-mu, invSigma * (x-mu));
  let b = -0.5 * a - ln_2pi - 0.5 * log(det);
  return b;
}

fn g2d_pdf(g: mat3x3f, x: vec2f) -> f32 {
  return exp(g2d_logpdf(g, x));
}

// mix two gaussians (used in the experiment page)
fn mixture(u: f32, m0: f32, s0: f32, m1: f32, s1: f32) -> f32 {
  let r = random_uniform(0.0, 1.0);
  if (r <= u) {
    return random_normal(m0, s0);
  } else {
    return random_normal(m1, s1);
  }
}
