//! Scalar math helpers (pbrt-v4 `src/pbrt/util/math.h`).

/// The smaller of `a` and `b`, with C++ `std::min` semantics: `b` if `b < a`,
/// otherwise `a`. If either is NaN the comparison is false and `a` is
/// returned; `min(-0.0, 0.0)` is `-0.0` and `min(0.0, -0.0)` is `0.0`.
///
/// pbrt-v4 calls `std::min` throughout; this keeps its results exactly,
/// unlike `f32::min`, which prefers the non-NaN operand.
#[inline]
#[must_use]
pub fn min<T: PartialOrd>(a: T, b: T) -> T {
    if b < a { b } else { a }
}

/// The larger of `a` and `b`, with C++ `std::max` semantics: `b` if `a < b`,
/// otherwise `a`. If either is NaN the comparison is false and `a` is
/// returned.
#[inline]
#[must_use]
pub fn max<T: PartialOrd>(a: T, b: T) -> T {
    if a < b { b } else { a }
}

use core::ops::{Add, AddAssign, Mul, Sub};

use crate::float::{exponent, floor, fma, significand, sqrt};
use crate::transcendental::erf;
use crate::{acosf, asinf, logf, sinf};

/// π, from `core`.
///
/// pbrt-v4 `Pi`.
pub const PI: f32 = core::f32::consts::PI;
/// 1/π.
///
/// pbrt-v4 `InvPi`.
pub const INV_PI: f32 = core::f32::consts::FRAC_1_PI;
/// 1/(2π).
///
/// pbrt-v4 `Inv2Pi`.
pub const INV_2_PI: f32 = core::f32::consts::FRAC_1_PI * 0.5;
/// 1/(4π).
///
/// pbrt-v4 `Inv4Pi`.
pub const INV_4_PI: f32 = core::f32::consts::FRAC_1_PI * 0.25;
/// π/2.
///
/// pbrt-v4 `PiOver2`.
pub const PI_OVER_2: f32 = core::f32::consts::FRAC_PI_2;
/// π/4.
///
/// pbrt-v4 `PiOver4`.
pub const PI_OVER_4: f32 = core::f32::consts::FRAC_PI_4;
/// √2.
///
/// pbrt-v4 `Sqrt2`.
pub const SQRT_2: f32 = core::f32::consts::SQRT_2;

/// Linear interpolation `(1 − x)·a + x·b`, without FMA.
///
/// PBR 4e §B.2.1, pbrt-v4 `Lerp`.
#[inline]
#[must_use]
pub fn lerp(x: f32, a: f32, b: f32) -> f32 {
    (1.0 - x) * a + x * b
}

/// `val` clamped to `[low, high]`.
///
/// PBR 4e §B.2.1, pbrt-v4 `Clamp`.
#[inline]
#[must_use]
pub fn clamp<T: PartialOrd>(val: T, low: T, high: T) -> T {
    if val < low {
        low
    } else if val > high {
        high
    } else {
        val
    }
}

/// `v · v`.
///
/// PBR 4e §B.2.1, pbrt-v4 `Sqr`.
#[inline]
#[must_use]
pub fn sqr<T: Mul<Output = T> + Copy>(v: T) -> T {
    v * v
}

/// Degrees to radians.
///
/// PBR 4e §B.2.1, pbrt-v4 `Radians`.
#[inline]
#[must_use]
pub fn radians(deg: f32) -> f32 {
    (PI / 180.0) * deg
}

/// Radians to degrees.
///
/// PBR 4e §B.2.1, pbrt-v4 `Degrees`.
#[inline]
#[must_use]
pub fn degrees(rad: f32) -> f32 {
    (180.0 / PI) * rad
}

/// Hermite smooth step of `x` from `a` to `b`.
///
/// PBR 4e §B.2.1, pbrt-v4 `SmoothStep`.
#[must_use]
pub fn smooth_step(x: f32, a: f32, b: f32) -> f32 {
    #[expect(
        clippy::float_cmp,
        reason = "exact test for a degenerate range, as in pbrt-v4"
    )]
    if a == b {
        return if x < a { 0.0 } else { 1.0 };
    }
    debug_assert!(a < b, "smooth_step: a = {a} is not below b = {b}");
    let t = clamp((x - a) / (b - a), 0.0, 1.0);
    t * t * (3.0 - 2.0 * t)
}

/// `√x` with `x` clamped to zero, for arguments that are only negative
/// through rounding error.
///
/// PBR 4e §B.2.1, pbrt-v4 `SafeSqrt`.
#[inline]
#[must_use]
pub fn safe_sqrt(x: f32) -> f32 {
    debug_assert!(x >= -1e-3, "safe_sqrt: {x} is too negative");
    sqrt(max(0.0, x))
}

/// `asin` with the argument clamped to `[−1, 1]`.
///
/// PBR 4e §B.2.1, pbrt-v4 `SafeASin`.
#[inline]
#[must_use]
pub fn safe_asin(x: f32) -> f32 {
    debug_assert!(
        (-1.0001..=1.0001).contains(&x),
        "safe_asin: {x} out of range"
    );
    asinf(clamp(x, -1.0, 1.0))
}

/// `acos` with the argument clamped to `[−1, 1]`.
///
/// PBR 4e §B.2.1, pbrt-v4 `SafeACos`.
#[inline]
#[must_use]
pub fn safe_acos(x: f32) -> f32 {
    debug_assert!(
        (-1.0001..=1.0001).contains(&x),
        "safe_acos: {x} out of range"
    );
    acosf(clamp(x, -1.0, 1.0))
}

/// `sin(x) / x`, exactly 1 where `1 − x²` rounds to 1.
///
/// PBR 4e §B.2.1, pbrt-v4 `SinXOverX`.
#[must_use]
pub fn sin_x_over_x(x: f32) -> f32 {
    #[expect(clippy::float_cmp, reason = "the exact test of pbrt-v4")]
    if 1.0 - x * x == 1.0 {
        return 1.0;
    }
    sinf(x) / x
}

/// The polynomial `c[0] + c[1]·t + c[2]·t² + …`, by Horner's rule with FMA.
///
/// PBR 4e §B.2.2, pbrt-v4 `EvaluatePolynomial`.
#[must_use]
pub fn evaluate_polynomial<const N: usize>(t: f32, c: [f32; N]) -> f32 {
    const { assert!(N > 0, "evaluate_polynomial needs a coefficient") };
    c.iter()
        .rev()
        .copied()
        .reduce(|acc, ci| fma(t, acc, ci))
        .unwrap_or(0.0)
}

/// An approximation of `eˣ` from a cubic in `2^frac(x / ln 2)`.
///
/// PBR 4e §B.2.2, pbrt-v4 `FastExp`.
#[must_use]
pub fn fast_exp(x: f32) -> f32 {
    let xp = x * core::f32::consts::LOG2_E;
    let fxp = floor(xp);
    let f = xp - fxp;
    #[expect(
        clippy::cast_possible_truncation,
        reason = "out of range saturates to ±0/∞ below"
    )]
    let i = fxp as i32;
    let two_to_f = evaluate_polynomial(f, [1.0, 0.695_556_9, 0.226_173_56, 0.078_145_57]);
    let exp = exponent(two_to_f).saturating_add(i);
    if exp < -126 {
        return 0.0;
    }
    if exp > 127 {
        return f32::INFINITY;
    }
    let bits = two_to_f.to_bits() & 0b1000_0000_0111_1111_1111_1111_1111_1111;
    #[expect(clippy::cast_sign_loss, reason = "exp + 127 is in 1..=254")]
    let biased = (exp + 127) as u32;
    f32::from_bits(bits | (biased << 23))
}

/// The normal density with mean `mu` and standard deviation `sigma`.
///
/// PBR 4e §B.2.2, pbrt-v4 `Gaussian`.
#[must_use]
pub fn gaussian(x: f32, mu: f32, sigma: f32) -> f32 {
    1.0 / sqrt(2.0 * PI * sigma * sigma) * fast_exp(-sqr(x - mu) / (2.0 * sigma * sigma))
}

/// The integral of [`gaussian`] over `[x0, x1]`.
///
/// PBR 4e §B.2.2, pbrt-v4 `GaussianIntegral`.
#[must_use]
pub fn gaussian_integral(x0: f32, x1: f32, mu: f32, sigma: f32) -> f32 {
    debug_assert!(sigma > 0.0, "gaussian_integral: sigma = {sigma}");
    let sigma_root2 = sigma * SQRT_2;
    0.5 * (erf((mu - x0) / sigma_root2) - erf((mu - x1) / sigma_root2))
}

/// The inverse error function, from Giles' single-precision approximation.
///
/// PBR 4e §B.2.2, pbrt-v4 `ErfInv`.
#[must_use]
pub fn erf_inv(a: f32) -> f32 {
    const TAIL: [f32; 9] = [
        8.400_165e-1,
        -2.646_461_4e-1,
        4.831_858e-3,
        3.026_988_1e-3,
        3.935_529_7e-4,
        2.841_089_6e-5,
        1.221_503_3e-6,
        2.932_431e-8,
        3.036_975_7e-10,
    ];
    const CENTRAL: [f32; 10] = [
        8.862_269e-1,
        -2.320_154_8e-1,
        1.153_925_6e-2,
        2.314_687e-3,
        -1.476_977e-4,
        -5.615_319_6e-5,
        1.129_626_3e-7,
        1.227_754e-6,
        1.432_860_6e-7,
        5.438_778_3e-9,
    ];
    let t = logf(max(fma(a, -a, 1.0), f32::MIN_POSITIVE));
    debug_assert!(t.is_finite(), "erf_inv: log term {t} for {a}");
    let p = if t.abs() > 6.125 {
        evaluate_polynomial(t, TAIL)
    } else {
        evaluate_polynomial(t, CENTRAL)
    };
    a * p
}

/// The index `i` in `[0, size − 2]` such that `pred(i)` holds and
/// `pred(i + 1)` does not, by bisection over a predicate that is true then
/// false. Out of range it clamps to the first or last interval.
///
/// PBR 4e §B.2.3, pbrt-v4 `FindInterval`.
#[must_use]
pub fn find_interval(size: usize, pred: impl Fn(usize) -> bool) -> usize {
    debug_assert!(size >= 2, "find_interval: size {size} has no interval");
    let mut remaining = size.saturating_sub(2);
    let mut first = 1;
    while remaining > 0 {
        let half = remaining >> 1;
        let middle = first + half;
        if pred(middle) {
            first = middle + 1;
            remaining -= half + 1;
        } else {
            remaining = half;
        }
    }
    clamp(first.saturating_sub(1), 0, size.saturating_sub(2))
}

/// Powers of two of integers.
///
/// PBR 4e §B.2.4.
pub trait PowerOf2: Copy {
    /// pbrt-v4 `IsPowerOf2`.
    fn is_power_of_2(self) -> bool;
    /// The smallest power of two not below `self`.
    ///
    /// pbrt-v4 `RoundUpPow2`.
    #[must_use]
    fn round_up_pow2(self) -> Self;
}

macro_rules! impl_power_of_2_unsigned {
    ($($t:ty),*) => {$(
        impl PowerOf2 for $t {
            #[inline]
            fn is_power_of_2(self) -> bool {
                self != 0 && (self & (self.wrapping_sub(1))) == 0
            }

            /// pbrt-v4 smears the top bit of `v − 1` down with five shift-or
            /// steps; one count of leading zeros gives the same smeared value
            /// (the reference in `tests/umbra_math/reference/math.rs` keeps the
            /// loop).
            #[inline]
            fn round_up_pow2(self) -> Self {
                let below = self.wrapping_sub(1);
                let smeared = <$t>::MAX.checked_shr(below.leading_zeros()).unwrap_or(0);
                smeared.wrapping_add(1)
            }
        }
    )*};
}
impl_power_of_2_unsigned!(u32, u64);

/// The signed types, on their bits as pbrt-v4's two's-complement arithmetic
/// works on them.
macro_rules! impl_power_of_2_signed {
    ($($t:ty),*) => {$(
        impl PowerOf2 for $t {
            #[inline]
            fn is_power_of_2(self) -> bool {
                self.cast_unsigned().is_power_of_2()
            }

            #[inline]
            fn round_up_pow2(self) -> Self {
                self.cast_unsigned().round_up_pow2().cast_signed()
            }
        }
    )*};
}
impl_power_of_2_signed!(i32, i64);

/// Whether `v` is a power of two.
///
/// PBR 4e §B.2.4, pbrt-v4 `IsPowerOf2`.
#[inline]
#[must_use]
pub fn is_power_of_2<T: PowerOf2>(v: T) -> bool {
    v.is_power_of_2()
}

/// The smallest power of two not below `v`.
///
/// PBR 4e §B.2.4, pbrt-v4 `RoundUpPow2`.
#[inline]
#[must_use]
pub fn round_up_pow2<T: PowerOf2>(v: T) -> T {
    v.round_up_pow2()
}

/// The base-2 logarithm, as an integer.
///
/// PBR 4e §B.2.4.
pub trait Log2Int: Copy {
    /// pbrt-v4 `Log2Int`.
    fn log2_int(self) -> i32;
}

macro_rules! impl_log2_int {
    ($($t:ty),*) => {$(
        impl Log2Int for $t {
            /// `⌊log₂ v⌋`; −1 for zero.
            #[inline]
            fn log2_int(self) -> i32 {
                #[expect(clippy::cast_possible_wrap, reason = "at most 64")]
                let lz = self.leading_zeros() as i32;
                #[expect(clippy::cast_possible_wrap, reason = "at most 64")]
                let bits = <$t>::BITS as i32;
                bits - 1 - lz
            }
        }
    )*};
}
impl_log2_int!(u32, u64);

impl Log2Int for i32 {
    #[inline]
    fn log2_int(self) -> i32 {
        self.cast_unsigned().log2_int()
    }
}

impl Log2Int for i64 {
    #[inline]
    fn log2_int(self) -> i32 {
        self.cast_unsigned().log2_int()
    }
}

impl Log2Int for f32 {
    /// `log₂ v` rounded to the nearest integer, from the exponent and
    /// significand bits.
    #[inline]
    fn log2_int(self) -> i32 {
        debug_assert!(self > 0.0, "log2_int of {self}");
        if self < 1.0 {
            -log2_int_at_least_one(1.0 / self)
        } else {
            log2_int_at_least_one(self)
        }
    }
}

/// pbrt-v4 `Log2Int(float)` for `v ≥ 1`, which then needs no recursion.
fn log2_int_at_least_one(v: f32) -> i32 {
    // The significand of 2^1.5: halfway between two exponents in log space.
    const MID_SIGNIFICAND: u32 = 0b0000_0000_0011_0101_0000_0100_1111_0011;
    exponent(v) + i32::from(significand(v) >= MID_SIGNIFICAND)
}

/// The integer base-2 logarithm of `v`.
///
/// PBR 4e §B.2.4, pbrt-v4 `Log2Int`.
#[inline]
#[must_use]
pub fn log2_int<T: Log2Int>(v: T) -> i32 {
    v.log2_int()
}

/// A value with the rounding error of the operation that produced it.
///
/// PBR 4e §B.2.9, pbrt-v4 `CompensatedFloat`.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct CompensatedFloat {
    /// The rounded result.
    pub v: f32,
    /// The rounding error: the exact result is `v + err`.
    pub err: f32,
}

impl From<CompensatedFloat> for f32 {
    fn from(c: CompensatedFloat) -> Self {
        c.v + c.err
    }
}

/// `a · b` with its exact rounding error, by FMA.
///
/// PBR 4e §B.2.9, pbrt-v4 `TwoProd`.
#[inline]
#[must_use]
pub fn two_prod(a: f32, b: f32) -> CompensatedFloat {
    let ab = a * b;
    CompensatedFloat {
        v: ab,
        err: fma(a, b, -ab),
    }
}

/// `a + b` with its exact rounding error (Knuth's `TwoSum`).
///
/// PBR 4e §B.2.9, pbrt-v4 `TwoSum`.
#[inline]
#[must_use]
pub fn two_sum(a: f32, b: f32) -> CompensatedFloat {
    let s = a + b;
    let delta = s - a;
    CompensatedFloat {
        v: s,
        err: (a - (s - delta)) + (b - delta),
    }
}

/// The dot product `Σ aᵢ·bᵢ` of the pairs `(aᵢ, bᵢ)`, compensated so it is
/// nearly as accurate as if computed in twice the precision (Graillat et al.).
/// The terms are combined from the last pair to the first, as pbrt-v4's
/// variadic recursion does.
///
/// PBR 4e §B.2.9, pbrt-v4 `InnerProduct`.
#[must_use]
pub fn inner_product<const N: usize>(terms: [(f32, f32); N]) -> f32 {
    const { assert!(N > 0, "inner_product needs a term") };
    let compensated = terms
        .iter()
        .rev()
        .map(|&(a, b)| two_prod(a, b))
        .reduce(|tp, ab| {
            let sum = two_sum(ab.v, tp.v);
            CompensatedFloat {
                v: sum.v,
                err: ab.err + (tp.err + sum.err),
            }
        })
        .unwrap_or_default();
    f32::from(compensated)
}

/// The products `a·b − c·d` and `a·b + c·d`, computed with FMA so that the
/// cancellation in them does not lose precision (Kahan's algorithm), or, for
/// [`Interval`](crate::Interval), conservatively bounded.
///
/// PBR 4e §B.2.9 and §6.8.2.
pub trait Products: Copy {
    /// pbrt-v4 `DifferenceOfProducts`.
    #[must_use]
    fn difference_of_products(a: Self, b: Self, c: Self, d: Self) -> Self;
    /// pbrt-v4 `SumOfProducts`.
    #[must_use]
    fn sum_of_products(a: Self, b: Self, c: Self, d: Self) -> Self;
}

impl Products for f32 {
    #[inline]
    fn difference_of_products(a: Self, b: Self, c: Self, d: Self) -> Self {
        let cd = c * d;
        let difference = fma(a, b, -cd);
        let error = fma(-c, d, cd);
        difference + error
    }

    #[inline]
    fn sum_of_products(a: Self, b: Self, c: Self, d: Self) -> Self {
        let cd = c * d;
        let sum = fma(a, b, cd);
        let error = fma(c, d, -cd);
        sum + error
    }
}

/// `a·b − c·d` without catastrophic cancellation.
///
/// PBR 4e §B.2.9, pbrt-v4 `DifferenceOfProducts`.
#[inline]
#[must_use]
pub fn difference_of_products<T: Products>(a: T, b: T, c: T, d: T) -> T {
    T::difference_of_products(a, b, c, d)
}

/// `a·b + c·d` without catastrophic cancellation.
///
/// PBR 4e §B.2.9, pbrt-v4 `SumOfProducts`.
#[inline]
#[must_use]
pub fn sum_of_products<T: Products>(a: T, b: T, c: T, d: T) -> T {
    T::sum_of_products(a, b, c, d)
}

/// The two roots `t0 ≤ t1` of `a·t² + b·t + c`, or `None` when they are not
/// real.
///
/// PBR 4e §B.2.10, pbrt-v4 `Quadratic`.
#[must_use]
pub fn quadratic(a: f32, b: f32, c: f32) -> Option<(f32, f32)> {
    if a == 0.0 {
        if b == 0.0 {
            return None;
        }
        let t = -c / b;
        return Some((t, t));
    }
    let discrim = difference_of_products(b, b, 4.0 * a, c);
    if discrim < 0.0 {
        return None;
    }
    let root_discrim = sqrt(discrim);
    let q = -0.5 * (b + root_discrim.copysign(b));
    let (t0, t1) = (q / a, c / q);
    Some(if t0 > t1 { (t1, t0) } else { (t0, t1) })
}

/// A running sum with Kahan compensation, for long sums of values of very
/// different magnitude.
///
/// PBR 4e §B.2.9, pbrt-v4 `CompensatedSum`.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct CompensatedSum<T> {
    sum: T,
    c: T,
}

impl<T: Copy + Default> CompensatedSum<T> {
    /// A sum that starts at `v`.
    #[must_use]
    pub fn new(v: T) -> Self {
        Self {
            sum: v,
            c: T::default(),
        }
    }

    /// The sum so far.
    #[must_use]
    pub fn value(self) -> T {
        self.sum
    }
}

impl<T: Copy + Add<Output = T> + Sub<Output = T>> AddAssign<T> for CompensatedSum<T> {
    fn add_assign(&mut self, v: T) {
        let delta = v - self.c;
        let new_sum = self.sum + delta;
        self.c = (new_sum - self.sum) - delta;
        self.sum = new_sum;
    }
}

/// The element types of vectors, points and normals that support lengths,
/// normalisation and products: `f32`, and `Interval` for the error-bounded
/// `Vector3fi`/`Point3fi`.
///
/// It stands in for the C++ overload sets pbrt-v4's generic tuple code calls
/// (`Sqr`, `std::sqrt`, `std::abs`, `FMA`).
pub trait Real:
    Products
    + Add<Output = Self>
    + Sub<Output = Self>
    + Mul<Output = Self>
    + core::ops::Div<Output = Self>
    + core::ops::Neg<Output = Self>
{
    /// pbrt-v4 `Sqr`.
    #[must_use]
    fn sqr(self) -> Self;
    /// pbrt-v4 `std::sqrt`, `Sqrt(Interval)`.
    #[must_use]
    fn sqrt(self) -> Self;
    /// pbrt-v4 `std::abs`, `Abs(Interval)`.
    #[must_use]
    fn abs(self) -> Self;
    /// pbrt-v4 `FMA`.
    #[must_use]
    fn fma(a: Self, b: Self, c: Self) -> Self;
}

impl Real for f32 {
    #[inline]
    fn sqr(self) -> Self {
        self * self
    }

    #[inline]
    fn sqrt(self) -> Self {
        sqrt(self)
    }

    #[inline]
    fn abs(self) -> Self {
        self.abs()
    }

    #[inline]
    fn fma(a: Self, b: Self, c: Self) -> Self {
        fma(a, b, c)
    }
}
