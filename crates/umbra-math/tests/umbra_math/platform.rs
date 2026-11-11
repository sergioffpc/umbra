//! Each safe C math wrapper of `umbra_math`, the `f32` ones at the crate root
//! and the `f64` ones in `double`, against known values (ADR 0007):
//! the C function's result within [`BOUND`] ULPs of a 17-digit reference,
//! rounded to `f32` for the `f32` functions. A wrong FFI signature (an `f64`
//! read as an `f32`, two arguments swapped, `rintf` for `roundf`) is off by
//! far more, so this catches it, where Miri, which trusts the `extern`
//! declaration, cannot.

use core::f64::consts::{E, FRAC_PI_3, FRAC_PI_6, LN_2, SQRT_2};

use umbra_math::double;
use umbra_math::{
    acosf, asinf, atan2f, atanhf, ceilf, cosf, coshf, erff, exp2f, expf, floorf, fmaf, logf, powf,
    roundf, sincosf, sinf, sqrtf, tanf,
};

use crate::accuracy::{narrow, ulps};

/// The error allowed, in ULPs: no less than glibc's manual states for these
/// functions on x86-64.
const BOUND: u32 = 2;

/// `3π/4`, the `atan2` of `(1, −1)`.
const THREE_PI_OVER_4: f64 = 2.356_194_490_192_345;

/// Asserts that the `f32` result `ours` is within [`BOUND`] ULPs of `exact`.
#[track_caller]
fn assert_f32(what: &str, ours: f32, exact: f64) {
    let error = ulps(ours, narrow(exact));
    assert!(
        error <= BOUND,
        "{what} = {ours:e}, {error} ULPs from {exact:e}"
    );
}

/// The error of `ours` against `exact` in `f64` ULPs, of one sign.
fn ulps_f64(ours: f64, exact: f64) -> u64 {
    let ordinal = |v: f64| i128::from(v.to_bits() & 0x7fff_ffff_ffff_ffff);
    if ours.is_sign_negative() == exact.is_sign_negative() {
        u64::try_from((ordinal(ours) - ordinal(exact)).unsigned_abs()).unwrap_or(u64::MAX)
    } else {
        u64::MAX
    }
}

/// Asserts that the `f64` result `ours` is within [`BOUND`] ULPs of `exact`.
#[track_caller]
fn assert_f64(what: &str, ours: f64, exact: f64) {
    let error = ulps_f64(ours, exact);
    assert!(
        error <= u64::from(BOUND),
        "{what} = {ours:e}, {error} ULPs from {exact:e}"
    );
}

#[test]
fn trigonometric_f32() {
    assert_f32("sinf(1)", sinf(1.0), 0.841_470_984_807_896_5);
    assert_f32("cosf(1)", cosf(1.0), 0.540_302_305_868_139_8);
    assert_f32("tanf(1)", tanf(1.0), 1.557_407_724_654_902_3);
    assert_f32("asinf(0.5)", asinf(0.5), FRAC_PI_6);
    assert_f32("acosf(0.5)", acosf(0.5), FRAC_PI_3);
    assert_f32("atan2f(1, -1)", atan2f(1.0, -1.0), THREE_PI_OVER_4);
}

#[test]
fn sincosf_writes_both() {
    let (sin, cos) = sincosf(1.0);
    assert_f32("sincosf(1).0", sin, 0.841_470_984_807_896_5);
    assert_f32("sincosf(1).1", cos, 0.540_302_305_868_139_8);
}

#[test]
fn hyperbolic_f32() {
    assert_f32("coshf(1)", coshf(1.0), 1.543_080_634_815_243_7);
    assert_f32("atanhf(0.5)", atanhf(0.5), 0.549_306_144_334_054_9);
}

#[test]
fn exponential_f32() {
    assert_f32("expf(1)", expf(1.0), E);
    assert_f32("exp2f(0.5)", exp2f(0.5), SQRT_2);
    assert_f32("logf(2)", logf(2.0), LN_2);
    assert_f32("powf(2, 0.5)", powf(2.0, 0.5), SQRT_2);
    assert_f32("powf(0.5, 2)", powf(0.5, 2.0), 0.25);
    assert_f32("erff(0.5)", erff(0.5), 0.520_499_877_813_046_5);
}

#[test]
fn exact_f32() {
    let tiny = f32::EPSILON;
    assert_f32("sqrtf(2)", sqrtf(2.0), SQRT_2);
    // (1 + ε)(1 − ε) − 1 = −ε², which one rounding keeps and two lose.
    assert_f32(
        "fmaf(1 + ε, 1 - ε, -1)",
        fmaf(1.0 + tiny, 1.0 - tiny, -1.0),
        -f64::from(tiny) * f64::from(tiny),
    );
    assert_f32("floorf(-1.5)", floorf(-1.5), -2.0);
    assert_f32("ceilf(-1.5)", ceilf(-1.5), -1.0);
    // Halfway cases round away from zero, not to even.
    assert_f32("roundf(2.5)", roundf(2.5), 3.0);
    assert_f32("roundf(-0.5)", roundf(-0.5), -1.0);
}

#[test]
fn trigonometric_f64() {
    assert_f64("sin(1)", double::sin(1.0), 0.841_470_984_807_896_5);
    assert_f64("cos(1)", double::cos(1.0), 0.540_302_305_868_139_8);
    assert_f64("acos(0.5)", double::acos(0.5), FRAC_PI_3);
    assert_f64("atan2(1, -1)", double::atan2(1.0, -1.0), THREE_PI_OVER_4);
    assert_f64("hypot(3, 4)", double::hypot(3.0, 4.0), 5.0);
}

#[test]
fn exponential_f64() {
    assert_f64("exp(1)", double::exp(1.0), E);
    assert_f64("log(2)", double::log(2.0), LN_2);
    assert_f64("pow(2, 0.5)", double::pow(2.0, 0.5), SQRT_2);
    assert_f64("pow(0.5, 2)", double::pow(0.5, 2.0), 0.25);
    assert_f64("cbrt(2)", double::cbrt(2.0), 1.259_921_049_894_873_2);
    assert_f64("cbrt(-27)", double::cbrt(-27.0), -3.0);
    assert_f64("erf(0.5)", double::erf(0.5), 0.520_499_877_813_046_5);
}

#[test]
fn lgamma_of_known_values() {
    // ln Γ(1/2) = ln √π, ln Γ(10) = ln 9!.
    assert_f64("lgamma(0.5)", double::lgamma(0.5), 0.572_364_942_924_700_1);
    assert_f64("lgamma(10)", double::lgamma(10.0), 12.801_827_480_081_469);
}

#[test]
fn exact_f64() {
    let tiny = f64::EPSILON;
    assert_f64("sqrt(2)", double::sqrt(2.0), SQRT_2);
    assert_f64(
        "fma(1 + ε, 1 - ε, -1)",
        double::fma(1.0 + tiny, 1.0 - tiny, -1.0),
        -tiny * tiny,
    );
}
