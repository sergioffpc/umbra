//! `exp` and `pow` of four `f32` lanes, evaluated in `f64` in one AVX
//! register (ADR 0022).
//!
//! The four lanes are widened to `f64`, which is exact and turns every `f32`
//! subnormal into a normal `f64`, and narrowed again at the end with one
//! rounding to nearest, which also produces the `f32` subnormals,
//! overflows and underflows. In between, Taylor polynomials whose truncation
//! error is below 2⁻²⁷ relative do the work, so each lane is within 1 ULP of
//! the correctly rounded result; `tests/umbra_math/accuracy/` checks it.
//!
//! The functions are compiled for AVX2 and FMA with `target_feature`, so the
//! intrinsics are safe calls; the backend (`super`) calls them, which is
//! sound because the crate requires `x86-64-v3` (`lib.rs`). They move no
//! memory: every intrinsic takes and returns registers.

use core::arch::x86_64::{
    __m128, __m256d, __m256i, _mm256_add_epi64, _mm256_add_pd, _mm256_castpd_si256,
    _mm256_castsi256_pd, _mm256_cvtpd_ps, _mm256_cvtps_pd, _mm256_div_pd, _mm256_fmadd_pd,
    _mm256_max_pd, _mm256_min_pd, _mm256_mul_pd, _mm256_or_si256, _mm256_set1_epi64x,
    _mm256_set1_pd, _mm256_slli_epi64, _mm256_srli_epi64, _mm256_sub_epi64, _mm256_sub_pd,
};
use core::f64::consts::{LN_2, LOG2_E};

/// 1.5 · 2⁵². Adding it to an `f64` of magnitude below 2⁵¹ rounds that to an
/// integer, to nearest with ties to even, and leaves the integer, in two's
/// complement, in the low bits of the significand.
const ROUND: f64 = 6_755_399_441_055_744.0;

/// The `f64` exponent bias.
const BIAS: i64 = 1023;

/// `eᵍ` in powers of `g`: the Taylor series to `g⁷`, whose remainder on
/// `|g| ≤ ln 2 / 2` is below 2⁻²⁷ relative.
const EXP: [f64; 8] = [
    1.0,
    1.0,
    1.0 / 2.0,
    1.0 / 6.0,
    1.0 / 24.0,
    1.0 / 120.0,
    1.0 / 720.0,
    1.0 / 5_040.0,
];

/// `ln(m) / 2t` in powers of `t²`, for `t = (m − 1)/(m + 1)`: the series of
/// `atanh(t)/t` to `t¹⁰`, whose remainder on `m` in `[√½, √2)`
/// (`|t| ≤ 3 − 2√2`) is below 2⁻³⁴ relative.
const LOG: [f64; 6] = [1.0, 1.0 / 3.0, 1.0 / 5.0, 1.0 / 7.0, 1.0 / 9.0, 1.0 / 11.0];

/// The bits of √½ as an `f64`: the significands of [`log2`] start there.
const SQRT_HALF_BITS: i64 = 0x3fe6_a09e_667f_3bcd;

/// `c[0] + c[1]·x + c[2]·x² + …` per lane, by Horner's rule with FMA.
#[inline]
#[target_feature(enable = "avx2,fma")]
fn polynomial<const N: usize>(x: __m256d, c: &[f64; N]) -> __m256d {
    const { assert!(N > 0, "polynomial needs a coefficient") };
    let mut acc = _mm256_set1_pd(c[N - 1]);
    for &ci in c[..N - 1].iter().rev() {
        acc = _mm256_fmadd_pd(acc, x, _mm256_set1_pd(ci));
    }
    acc
}

/// `2ᶻ` per lane. `z` is clamped to `[−152, 130]`, beyond which every
/// `f32` result is zero or infinite; a NaN stays NaN.
///
/// `z = n + f` with `n` the nearest integer and `|f| ≤ 1/2`, `2ᶠ = e^(f ln 2)`
/// by [`EXP`], and `2ⁿ` built from its exponent bits.
#[inline]
#[target_feature(enable = "avx2,fma")]
fn exp2(z: __m256d) -> __m256d {
    // `maxpd`/`minpd` return their second operand when either is NaN.
    let z = _mm256_max_pd(_mm256_set1_pd(-152.0), z);
    let z = _mm256_min_pd(_mm256_set1_pd(130.0), z);
    let shifted = _mm256_add_pd(z, _mm256_set1_pd(ROUND));
    let n = _mm256_sub_pd(shifted, _mm256_set1_pd(ROUND));
    // Exact: `n` is the integer nearest `z`.
    let f = _mm256_sub_pd(z, n);
    let p = polynomial(_mm256_mul_pd(f, _mm256_set1_pd(LN_2)), &EXP);
    let biased = _mm256_add_epi64(_mm256_castpd_si256(shifted), _mm256_set1_epi64x(BIAS));
    let two_to_n = _mm256_castsi256_pd(_mm256_slli_epi64::<52>(biased));
    _mm256_mul_pd(p, two_to_n)
}

/// The small non-negative integers `k` as `f64`: their bits under those of
/// 2⁵², minus 2⁵².
#[inline]
#[target_feature(enable = "avx2,fma")]
fn small_to_f64(k: __m256i) -> __m256d {
    const TWO_52: f64 = 4_503_599_627_370_496.0;
    let bits = _mm256_or_si256(k, _mm256_castpd_si256(_mm256_set1_pd(TWO_52)));
    _mm256_sub_pd(_mm256_castsi256_pd(bits), _mm256_set1_pd(TWO_52))
}

/// `log₂ a` per lane, for finite positive normal `a`.
///
/// `a = 2ᵏ · m` with `m` in `[√½, √2)`, split by integer arithmetic on the
/// bits, and `ln m = 2 atanh((m − 1)/(m + 1))` by [`LOG`]. The error is below
/// 2⁻³⁴ relative to `log₂ a`.
#[inline]
#[target_feature(enable = "avx2,fma")]
fn log2(a: __m256d) -> __m256d {
    let ix = _mm256_castpd_si256(a);
    // The exponent field of `ix − bits(√½)`, offset by 1024 to keep it
    // positive, is `k + 1024`.
    let offset = _mm256_sub_epi64(ix, _mm256_set1_epi64x(SQRT_HALF_BITS - (1024 << 52)));
    let k_plus_1024 = _mm256_srli_epi64::<52>(offset);
    let k = _mm256_sub_epi64(k_plus_1024, _mm256_set1_epi64x(1024));
    let m = _mm256_castsi256_pd(_mm256_sub_epi64(ix, _mm256_slli_epi64::<52>(k)));
    let one = _mm256_set1_pd(1.0);
    let t = _mm256_div_pd(_mm256_sub_pd(m, one), _mm256_add_pd(m, one));
    let series = _mm256_mul_pd(t, polynomial(_mm256_mul_pd(t, t), &LOG));
    let k = _mm256_sub_pd(small_to_f64(k_plus_1024), _mm256_set1_pd(1024.0));
    _mm256_fmadd_pd(series, _mm256_set1_pd(2.0 * LOG2_E), k)
}

/// `eˣ` per lane, within 1 ULP of the correctly rounded result:
/// `2^(x log₂ e)` by [`exp2`].
#[inline]
#[target_feature(enable = "avx2,fma")]
pub(super) fn exp(x: __m128) -> __m128 {
    let z = _mm256_mul_pd(_mm256_cvtps_pd(x), _mm256_set1_pd(LOG2_E));
    _mm256_cvtpd_ps(exp2(z))
}

/// `aᵉ` per lane for finite positive `a` and finite `e`, within 1 ULP of the
/// correctly rounded result: `2^(e log₂ a)` by [`log2`] and [`exp2`]. The
/// error of `log₂ a` is relative to it, so for every result that is finite
/// and non-zero in `f32` (`|e log₂ a| < 150`) it stays below 2⁻²⁶ relative.
/// Other lanes (zero, infinite, NaN) are garbage for the caller to replace.
#[inline]
#[target_feature(enable = "avx2,fma")]
pub(super) fn pow_positive(a: __m128, e: f32) -> __m128 {
    let z = _mm256_mul_pd(log2(_mm256_cvtps_pd(a)), _mm256_set1_pd(f64::from(e)));
    _mm256_cvtpd_ps(exp2(z))
}
