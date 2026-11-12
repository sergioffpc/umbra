//! The `nvptx64` backend of the scalar transcendentals (ADR 0028), in `f32`
//! only: `f64` runs at a small fraction of `f32`'s rate on consumer GPUs.
//!
//! The error bounds are those this module documents, not ADR 0022's 1 ULP:
//! the GPU promises statistical equality only (ADR 0026). `umbra-gpu`'s GPU
//! tests check each against glibc (#146).

use core::f32::consts::FRAC_1_PI;

use crate::float::fma;
use crate::libdevice::{atan2f, cosf, erff, fast_cosf, fast_sinf, sinf};

/// 1.5 · 2²³. Adding it to an `f32` of magnitude below 2²² rounds that to an
/// integer, to nearest with ties to even; subtracting it again gives the
/// integer as an `f32`.
const ROUND: f32 = 12_582_912.0;

/// `1/(2π)` rounded to `f32`.
const INV_2_PI: f32 = FRAC_1_PI * 0.5;

/// 2π in three parts, `A + B + C`, each rounded to `f32`; `A` has 8
/// significant bits. With them, `x − k·2π` is reduced to within half an ULP
/// of the remainder for every `|x| ≤` [`SIN_COS_LIMIT`].
const TWO_PI_A: f32 = 6.281_25;
/// 2π − [`TWO_PI_A`], rounded to `f32`.
const TWO_PI_B: f32 = 1.935_307_2e-3;
/// 2π − [`TWO_PI_A`] − [`TWO_PI_B`], rounded to `f32`.
const TWO_PI_C: f32 = 1.025_313_2e-11;

/// The magnitude up to which [`sin_cos`] reduces its argument itself, as the
/// x86-64 backend does; beyond it, `libdevice`'s `sinf` and `cosf`, whose
/// reduction is exact, take over.
const SIN_COS_LIMIT: f32 = 1_048_576.0;

/// Below this magnitude `sin r` rounds to `r` in `f32` (`r³/6` is less than
/// half an ULP of `r`), and [`sin_cos`] returns `r` itself. For a small `x`,
/// where `r = x` exactly, the sine keeps its relative accuracy, which the
/// absolute bound of `sin.approx.f32` does not promise; a large `x` that
/// reduces near a multiple of 2π keeps only the absolute bound.
const SIN_IS_ARGUMENT: f32 = 1.0 / 4_096.0;

/// 2²⁴, which scales every subnormal into the normal range, exactly.
const TINY_SCALE: f32 = 16_777_216.0;

/// The sine and cosine of `x` radians, with an absolute error below 2⁻²⁰
/// for `|x| ≤ 2²⁰`.
///
/// `x` is reduced to `r = x − k·2π`, with `|r| ≲ π`, by 2π in three parts
/// and FMAs, which costs at most 2⁻²² absolute. Then `sin.approx.f32` and
/// `cos.approx.f32` (by `libdevice`'s `__nv_fast_sinf` and
/// `__nv_fast_cosf`), whose absolute error on `[−π, π]` is at most 2⁻²¹·⁴¹
/// and 2⁻²¹·¹⁹. Below 2⁻¹² the sine is `r`, exact in `f32`. Beyond ±2²⁰ and
/// for infinities and NaN, they are `libdevice`'s `sinf` and `cosf`, within
/// 2 ULP.
///
/// Not in pbrt-v4, which calls `std::sin` and `std::cos` of the same angle.
#[must_use]
pub fn sin_cos(x: f32) -> (f32, f32) {
    if x.is_nan() || x.abs() > SIN_COS_LIMIT {
        return (sinf(x), cosf(x));
    }
    let k = fma(x, INV_2_PI, ROUND) - ROUND;
    let r = fma(-k, TWO_PI_C, fma(-k, TWO_PI_B, fma(-k, TWO_PI_A, x)));
    let sin = if r.abs() < SIN_IS_ARGUMENT {
        r
    } else {
        fast_sinf(r)
    };
    (sin, fast_cosf(r))
}

/// The angle of the point `(x, y)` from the positive x axis, in `[−π, π]`,
/// within 3 ULP: `libdevice`'s `atan2f`, with C's values at zeros,
/// infinities and NaN. Where both arguments are subnormal or zero, which
/// `atan2f` on the GPU takes for zeros, both are first scaled by 2²⁴, which
/// is exact and keeps the angle.
///
/// pbrt-v4 `std::atan2`.
#[inline]
#[must_use]
pub fn atan2(y: f32, x: f32) -> f32 {
    if y.abs() < f32::MIN_POSITIVE && x.abs() < f32::MIN_POSITIVE {
        // Both subnormal or zero, which the GPU's `atan2f` takes for zeros:
        // scaled into the normal range, exactly, the angle is the same.
        atan2f(y * TINY_SCALE, x * TINY_SCALE)
    } else {
        atan2f(y, x)
    }
}

/// The error function, within 2 ULP: `libdevice`'s `erff`.
///
/// pbrt-v4 `std::erf`.
#[inline]
#[must_use]
pub fn erf(x: f32) -> f32 {
    erff(x)
}
