//! Scalar transcendentals of Umbra's own, faster than the C routines they
//! replace (ADR 0022): [`sin_cos`], [`atan2`] and [`erf`].
//!
//! Each converts its `f32` argument to `f64`, which is exact, and evaluates a
//! polynomial whose error is below 2⁻³⁰ relative, with `f64` FMAs, whose
//! rounding errors are near 2⁻⁵². The polynomials are the interpolants of
//! the `libm` crate's `f64` routines, which Umbra used before glibc, at the
//! Chebyshev nodes of their interval, of the lowest degree that meets 2⁻³⁰
//! (for `erf`, one degree for all its segments). The `f32` result is then within 1 ULP of the correctly rounded
//! one, and correctly rounded for all but a few percent of the arguments.
//! `tests/umbra_math/accuracy/` checks the bound against glibc's `f64`
//! routines, exhaustively over `f32` for the unary functions. Special values
//! (signed zeros, infinities, NaN) give what C's `sinf`, `cosf`, `atan2f` and
//! `erff` give.
//!
//! The arithmetic is `f64` scalar code; the one instruction LLVM does not
//! emit on its own is the `f64` FMA, [`fma_f64`].

use core::f64::consts::{FRAC_2_PI, FRAC_PI_2, FRAC_PI_4, PI};

use crate::float::fma_f64;
use crate::{cosf, sinf};

/// 1.5 · 2⁵². Adding it to an `f64` of magnitude below 2⁵¹ rounds that to an
/// integer, to nearest with ties to even, and leaves the integer in the low
/// bits of the significand; subtracting it again gives the integer as an
/// `f64`.
const ROUND: f64 = 6_755_399_441_055_744.0;

/// `c[0] + c[1]·x + c[2]·x² + …`, by Horner's rule with FMA.
#[inline]
fn polynomial<const N: usize>(x: f64, c: [f64; N]) -> f64 {
    const { assert!(N > 0, "polynomial needs a coefficient") };
    c.iter()
        .rev()
        .copied()
        .reduce(|acc, ci| fma_f64(acc, x, ci))
        .unwrap_or(0.0)
}

/// The `f64` result rounded to the nearest `f32`.
#[inline]
fn narrow(x: f64) -> f32 {
    #[expect(
        clippy::cast_possible_truncation,
        reason = "the one rounding to f32, to nearest"
    )]
    let r = x as f32;
    r
}

/// `sin(r)/r` in powers of `r²`: the degree-4 interpolant at the Chebyshev
/// nodes of `r²` in `[0, (π/4)²]`, whose error is below 2⁻³⁷ relative.
const SIN: [f64; 5] = [
    9.999_999_999_956_513e-1,
    -1.666_666_663_145_047_7e-1,
    8.333_328_768_795_792e-3,
    -1.983_919_808_262_296e-4,
    2.717_307_468_908_936_4e-6,
];

/// `cos(r)` in powers of `r²`: the degree-4 interpolant at the Chebyshev
/// nodes of `r²` in `[0, (π/4)²]`, whose error is below 2⁻³³ relative.
const COS: [f64; 5] = [
    9.999_999_999_522_519e-1,
    -4.999_999_961_331_849e-1,
    4.166_661_654_296_987e-2,
    -1.388_661_347_730_571_8e-3,
    2.437_941_261_717_171e-5,
];

/// π/2 rounded to `f64`.
const FRAC_PI_2_HI: f64 = FRAC_PI_2;
/// π/2 − [`FRAC_PI_2_HI`], rounded to `f64`: with it, π/2 is known to about
/// 2⁻¹⁰⁷.
const FRAC_PI_2_LO: f64 = 6.123_233_995_736_766e-17;

/// The magnitude up to which [`sin_cos`] reduces its argument itself; beyond
/// it, glibc's exact reduction takes over. The quadrant must fit in the
/// low bits of [`ROUND`], and the error of π/2 in two parts, times the
/// quadrant, must stay far below the smallest remainder an `f32` there has.
const SIN_COS_LIMIT: f32 = 1_048_576.0;

/// The sine and cosine of `x` radians, each within 1 ULP of the correctly
/// rounded result: one argument reduction shared by both.
///
/// `x` is reduced to `r = x − k·π/2` with `|r| ≤ π/4`, with π/2 in two parts
/// and FMAs, then both are polynomials in `r²` evaluated in `f64`,
/// and the quadrant `k mod 4` picks and signs them. Beyond ±2²⁰ (and for
/// infinities and NaN) they are `sinf` and `cosf`.
///
/// Not in pbrt-v4, which calls `std::sin` and `std::cos` of the same angle.
#[must_use]
pub fn sin_cos(x: f32) -> (f32, f32) {
    if x.is_nan() || x.abs() > SIN_COS_LIMIT {
        return (sinf(x), cosf(x));
    }
    let x = f64::from(x);
    let shifted = x * FRAC_2_PI + ROUND;
    let k = shifted - ROUND;
    let r = fma_f64(-k, FRAC_PI_2_LO, fma_f64(-k, FRAC_PI_2_HI, x));
    let r2 = r * r;
    let (s, c) = (r * polynomial(r2, SIN), polynomial(r2, COS));
    // `k mod 4`, two's complement, from the low bits of the significand.
    let (sin, cos) = into_quadrant(s, c, shifted.to_bits());
    (narrow(sin), narrow(cos))
}

/// The sine and cosine of `r + k·π/2` from those of `r`, for the quadrant
/// `k mod 4` in the low two bits of `k`: `(s, c)`, `(c, −s)`, `(−s, −c)`,
/// `(−c, s)`. By bit operations, not branches, which random angles would
/// mispredict.
#[inline]
fn into_quadrant(s: f64, c: f64, k: u64) -> (f64, f64) {
    let swap = (k & 1).wrapping_neg();
    let (s, c) = (s.to_bits(), c.to_bits());
    let sin = (s & !swap) | (c & swap);
    let cos = (c & !swap) | (s & swap);
    let sin_sign = (k & 2) << 62;
    let cos_sign = (k.wrapping_add(1) & 2) << 62;
    (
        f64::from_bits(sin ^ sin_sign),
        f64::from_bits(cos ^ cos_sign),
    )
}

/// `atan(t)/t` in powers of `t²`: the degree-5 interpolant at the Chebyshev
/// nodes of `t²` in `[0, tan²(π/8)]`, whose error is below 2⁻³⁰ relative.
const ATAN: [f64; 6] = [
    9.999_999_993_676_982e-1,
    -3.333_330_677_112_112e-1,
    1.999_817_646_656_565_4e-1,
    -1.423_941_086_401_291_4e-1,
    1.056_892_083_089_207_8e-1,
    -6.023_963_287_668_914e-2,
];

/// tan(π/8) = √2 − 1.
const TAN_PI_8: f64 = 0.414_213_562_373_095_03;

/// `atan(t)` for `|t| ≤ tan(π/8)`.
#[inline]
fn atan_reduced(t: f64) -> f64 {
    t * polynomial(t * t, ATAN)
}

/// `atan(n/d)` for `0 ≤ n ≤ d`, in `[0, π/4]`. Above tan(π/8) the ratio is
/// moved down by `atan(n/d) = π/4 + atan((n − d)/(n + d))`, whose numerator
/// and denominator are exact for `f32` inputs. The choice is a select, not a
/// branch, which random angles would mispredict.
#[inline]
fn atan_of_ratio(n: f64, d: f64) -> f64 {
    if d == f64::INFINITY {
        return if n == f64::INFINITY { FRAC_PI_4 } else { 0.0 };
    }
    if d == 0.0 {
        return 0.0;
    }
    let above = n > TAN_PI_8 * d;
    let (num, den) = if above { (n - d, n + d) } else { (n, d) };
    let base = if above { FRAC_PI_4 } else { 0.0 };
    base + atan_reduced(num / den)
}

/// The angle of the point `(x, y)` from the positive x axis, in `[−π, π]`,
/// within 1 ULP of the correctly rounded result.
///
/// The smaller of `|x|` and `|y|` over the larger gives an angle in
/// `[0, π/4]`, from a polynomial in `f64` after one more reduction;
/// the octant moves it into place. Zeros and infinities give C's `atan2f`
/// values, NaN gives NaN.
///
/// pbrt-v4 `std::atan2`.
#[must_use]
pub fn atan2(y: f32, x: f32) -> f32 {
    if x.is_nan() || y.is_nan() {
        return x + y;
    }
    let (ax, ay) = (f64::from(x.abs()), f64::from(y.abs()));
    let steep = ay > ax;
    let (n, d) = if steep { (ax, ay) } else { (ay, ax) };
    let a = atan_of_ratio(n, d);
    let a = if steep { FRAC_PI_2 - a } else { a };
    let a = if x.is_sign_negative() { PI - a } else { a };
    narrow(a).copysign(y)
}

/// `erf` on the eight segments `[i/2, (i + 1)/2)`, each the degree-7
/// interpolant of the `libm` crate's `erf` (`f64`) at eight Chebyshev nodes,
/// whose error is below 2⁻³⁰ relative. The first is `erf(x)/x` in powers of `x²`, with
/// the nodes in `x²` on `[0, 1/4]`, which keeps the relative error small near
/// zero; the others are `erf(x)` in powers of `x − (i/2 + 1/4)`, with the
/// nodes in `x` on the segment.
const ERF: [[f64; 8]; 8] = [
    [
        1.128_379_167_095_510_6,
        -3.761_263_890_313_223e-1,
        1.128_379_166_743_176_4e-1,
        -2.686_616_962_753_874e-2,
        5.223_962_368_745_560_5e-3,
        -8.547_043_726_139_236e-4,
        1.199_429_389_089_345_9e-4,
        -1.338_589_936_494_827_3e-5,
    ],
    [
        7.111_556_333_764_832e-1,
        6.429_310_696_590_312e-1,
        -4.821_981_600_764_506_7e-1,
        2.678_855_698_788_140_7e-2,
        1.506_756_326_603_238e-1,
        -5.322_369_238_729_152e-2,
        -2.658_287_074_700_638_2e-2,
        1.794_584_683_375_433e-2,
    ],
    [
        9.229_001_289_239_191e-1,
        2.365_211_223_473_803_7e-1,
        -2.956_517_448_925_702e-1,
        1.675_358_462_413_534_3e-1,
        -6.132_017_034_474_302e-3,
        -4.718_514_643_968_774e-2,
        2.059_754_512_811_196e-2,
        3.732_252_604_095_265_3e-3,
    ],
    [
        9.866_716_711_386_647e-1,
        5.277_499_582_361_095e-2,
        -9.235_620_160_065_272e-2,
        9.015_733_924_429_98e-2,
        -4.810_553_438_318_266e-2,
        6.619_998_939_413_563e-3,
        9.049_599_490_026_594e-3,
        -5.947_049_459_791_742e-3,
    ],
    [
        9.985_372_832_943_709e-1,
        7.142_319_065_032_46e-3,
        -1.607_015_689_610_502_6e-2,
        2.172_453_164_118_692_8e-2,
        -1.908_825_665_628_910_4e-2,
        1.065_944_687_150_022_2e-2,
        -2.779_561_495_117_377_5e-3,
        -7.159_281_885_833_48e-4,
    ],
    [
        9.998_993_780_876_746e-1,
        5.862_772_557_660_711e-4,
        -1.612_267_450_365_578_7e-3,
        2.760_384_246_865_754_7e-3,
        -3.257_709_178_910_772_6e-3,
        2.756_164_572_360_831e-3,
        -1.667_864_104_092_586_8e-3,
        6.369_717_521_010_898e-4,
    ],
    [
        9.999_956_972_303_03e-1,
        2.918_902_228_724_040_7e-5,
        -9.486_933_247_249_141e-5,
        1.958_112_779_441_023_6e-4,
        -2.861_694_603_097_930_7e-4,
        3.136_720_962_402_251e-4,
        -2.737_581_089_604_646e-4,
        1.732_012_096_908_874_8e-4,
    ],
    [
        9.999_998_862_741_102e-1,
        8.814_314_010_896_496e-7,
        -3.306_069_862_762_939_2e-6,
        7.970_000_844_181_868e-6,
        -1.378_545_539_409_97e-5,
        1.834_053_091_442_911e-5,
        -2.069_192_669_296_171_5e-5,
        1.704_893_657_006_323_3e-5,
    ],
];

/// Where [`erf_magnitude`] stops: past ≈ 3.92, `erf` rounds to 1 in `f32`,
/// and the last segment ends at 4.
const ERF_ONE: f64 = 3.99;

/// `erf(a)` for `a ≥ 0`, not NaN. The segment is picked by index and the
/// variable by selects, not branches, which random arguments would
/// mispredict.
#[inline]
fn erf_magnitude(a: f64) -> f64 {
    let a = a.min(ERF_ONE);
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        reason = "2a is in [0, 8): the segment index"
    )]
    let i = ((a * 2.0) as usize).min(ERF.len() - 1);
    #[expect(clippy::cast_precision_loss, reason = "i is below 8")]
    let center = i as f64 * 0.5 + 0.25;
    let near_zero = i == 0;
    let u = if near_zero { a * a } else { a - center };
    let scale = if near_zero { a } else { 1.0 };
    scale * polynomial(u, ERF[i])
}

/// The error function, within 1 ULP of the correctly rounded result.
///
/// A Chebyshev interpolant on each half-unit segment up to 4, in `f64`,
/// and 1 beyond; odd in `x`.
///
/// pbrt-v4 `std::erf`.
#[must_use]
pub fn erf(x: f32) -> f32 {
    if x.is_nan() {
        return x;
    }
    narrow(erf_magnitude(f64::from(x.abs()))).copysign(x)
}
