//! [`sin_cos`], [`atan2`] and [`erf`] against `cmath::sin`, `cos`, `atan2`
//! and `erf` in `f64`.

use umbra_math::cmath;
use umbra_math::{atan2, erf, sin_cos};

use super::{COUNT, SPECIAL, SWEEP, assert_within, narrow, samples, ulps, worst_over_f32};

/// The larger error of the sine and the cosine of `x`.
fn sin_cos_error(x: f32) -> u32 {
    let (s, c) = sin_cos(x);
    let x64 = f64::from(x);
    ulps(s, narrow(cmath::sin(x64))).max(ulps(c, narrow(cmath::cos(x64))))
}

#[test]
fn sin_cos_within_bound() {
    assert_within(worst_over_f32(SWEEP, sin_cos_error), "sin_cos");
}

/// All 2³²; run on demand.
#[test]
#[ignore = "exhaustive sweep of every f32; run on demand"]
fn sin_cos_within_bound_exhaustive() {
    assert_within(worst_over_f32(1, sin_cos_error), "sin_cos");
}

/// The error of `erf(x)`.
fn erf_error(x: f32) -> u32 {
    ulps(erf(x), narrow(cmath::erf(f64::from(x))))
}

#[test]
fn erf_within_bound() {
    assert_within(worst_over_f32(SWEEP, erf_error), "erf");
}

/// All 2³²; run on demand.
#[test]
#[ignore = "exhaustive sweep of every f32; run on demand"]
fn erf_within_bound_exhaustive() {
    assert_within(worst_over_f32(1, erf_error), "erf");
}

/// The error of `atan2(y, x)`.
fn atan2_error(y: f32, x: f32) -> u32 {
    ulps(
        atan2(y, x),
        narrow(cmath::atan2(f64::from(y), f64::from(x))),
    )
}

/// Every angle of the shallow and the steep octants, both signs: one
/// coordinate swept over every [`SWEEP`]-th `f32` against ±1.
#[test]
fn atan2_within_bound_over_every_ratio() {
    for (one, what) in [(1.0, "atan2(t, 1)"), (-1.0, "atan2(t, −1)")] {
        assert_within(worst_over_f32(SWEEP, |t| atan2_error(t, one)), what);
        let steep = worst_over_f32(SWEEP, |t| atan2_error(one, t));
        assert_within(steep, &format!("{what}, swapped"));
    }
}

/// Random pairs, and every pair of special values.
#[test]
fn atan2_within_bound_on_samples() {
    let (y, x) = (samples(201, COUNT), samples(202, COUNT));
    let pairs = y.iter().zip(&x).map(|(&y, &x)| (y, x));
    let special = SPECIAL
        .iter()
        .flat_map(|&y| SPECIAL.iter().map(move |&x| (y, x)));
    for (y, x) in pairs.chain(special) {
        let e = atan2_error(y, x);
        assert!(e <= super::BOUND, "atan2({y:e}, {x:e}): {e} ULPs");
    }
}
