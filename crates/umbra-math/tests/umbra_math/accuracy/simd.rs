//! [`Vec4::exp`] and [`Vec4::pow`] against `libm::exp` and `libm::pow` in
//! `f64`, lane by lane. Each lane gets a different argument, so a lane
//! mixed up by the widening to `f64` and back shows.

use umbra_math::Vec4;

use super::{BOUND, COUNT, SPECIAL, SWEEP, assert_within, narrow, samples, ulps, worst_over_f32};

/// Four distinct arguments derived from `x`, which is the first.
fn lanes(x: f32) -> [f32; 4] {
    let bits = x.to_bits();
    [
        x,
        -x,
        f32::from_bits(bits ^ 0x0040_0000),
        f32::from_bits(bits.rotate_left(7)),
    ]
}

/// The largest error of `ours` over the lanes of `x` against `reference`.
fn worst_lane(x: [f32; 4], ours: Vec4, reference: impl Fn(f32) -> f32) -> u32 {
    x.iter()
        .zip(ours.to_array())
        .map(|(&x, r)| ulps(r, reference(x)))
        .max()
        .unwrap_or(0)
}

/// The reference `eˣ`.
fn exp_reference(x: f32) -> f32 {
    narrow(libm::exp(f64::from(x)))
}

/// The error of `exp` on the lanes derived from `x`.
fn exp_error(x: f32) -> u32 {
    let x = lanes(x);
    worst_lane(x, Vec4::from(x).exp(), exp_reference)
}

#[test]
fn exp_within_bound() {
    assert_within(worst_over_f32(SWEEP, exp_error), "Vec4::exp");
}

/// All 2³²; run on demand.
#[test]
#[ignore = "exhaustive sweep of every f32; run on demand"]
fn exp_within_bound_exhaustive() {
    assert_within(worst_over_f32(1, exp_error), "Vec4::exp");
}

/// The reference `xᵉ`.
fn pow_reference(x: f32, e: f32) -> f32 {
    narrow(libm::pow(f64::from(x), f64::from(e)))
}

/// The error of `pow(·, e)` on the lanes derived from `x`.
fn pow_error(x: f32, e: f32) -> u32 {
    let x = lanes(x);
    worst_lane(x, Vec4::from(x).pow(e), |x| pow_reference(x, e))
}

/// Exponents a renderer uses: gamma curves, roots, squares, odd and even
/// integers of both signs, large ones that only stay finite near 1.
const EXPONENTS: [f32; 16] = [
    2.2,
    1.0 / 2.2,
    0.5,
    2.0,
    3.0,
    -1.0,
    -2.0,
    -3.5,
    0.1,
    5.0,
    7.0,
    -7.0,
    100.0,
    -250.5,
    12_345.0,
    1.0e7,
];

/// Every base whose bits are a multiple of 16 · [`SWEEP`], for each of
/// [`EXPONENTS`].
#[test]
fn pow_within_bound_over_every_base() {
    for e in EXPONENTS {
        let worst = worst_over_f32(16 * SWEEP, |x| pow_error(x, e));
        assert_within(worst, &format!("Vec4::pow(·, {e})"));
    }
}

/// Random pairs, and every pair of special values.
#[test]
fn pow_within_bound_on_samples() {
    let (x, e) = (samples(203, COUNT), samples(204, COUNT));
    let pairs = x.iter().zip(&e).map(|(&x, &e)| (x, e));
    let special = SPECIAL
        .iter()
        .flat_map(|&x| SPECIAL.iter().map(move |&e| (x, e)));
    for (x, e) in pairs.chain(special) {
        let err = pow_error(x, e);
        assert!(err <= BOUND, "Vec4::pow({x:e}, {e:e}): {err} ULPs");
    }
}
