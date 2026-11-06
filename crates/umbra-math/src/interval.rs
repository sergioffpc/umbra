//! Interval arithmetic for conservative floating-point error bounds
//! (pbrt-v4 `Interval`, `src/pbrt/util/math.h`).
//!
//! The arithmetic runs as [`Vec4`](crate::Vec4) kernels ([`kernels`]): the
//! four bound products of a multiplication in one lane-wise multiply, their
//! outward rounding in one pass, and the extremes by two deinterleaves,
//! without a branch on the values. Each kernel computes the bits of its
//! scalar form in `tests/umbra_math/reference/interval.rs` (ADR 0007).

pub(crate) mod kernels;

use core::ops::{Add, AddAssign, Div, DivAssign, Mul, MulAssign, Neg, Sub, SubAssign};

use crate::float::{ceil, floor, significand};
use crate::math::{Products, max, min};

/// A closed interval `[low, high]` of `f32` that contains the exact result of
/// the computation that produced it. Each operation rounds its bounds outward.
///
/// PBR 4e §6.8.2, pbrt-v4 `Interval`.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Interval {
    low: f32,
    high: f32,
}

impl Interval {
    /// The interval between `a` and `b`, in either order.
    #[inline]
    #[must_use]
    pub fn new(a: f32, b: f32) -> Self {
        Self {
            low: min(a, b),
            high: max(a, b),
        }
    }

    /// The bounds as given, not sorted: pbrt-v4's direct member assignment
    /// in `FromValueAndError`.
    #[inline]
    #[must_use]
    pub(crate) const fn from_bounds(low: f32, high: f32) -> Self {
        Self { low, high }
    }

    /// The interval `[v − err, v + err]`, rounded outward; exactly `[v, v]`
    /// when `err` is zero.
    ///
    /// pbrt-v4 `Interval::FromValueAndError`.
    #[inline]
    #[must_use]
    pub fn from_value_and_error(v: f32, err: f32) -> Self {
        if err == 0.0 {
            Self { low: v, high: v }
        } else {
            kernels::from_value_and_error(v, err)
        }
    }

    /// pbrt-v4 `Interval::UpperBound`.
    #[inline]
    #[must_use]
    pub fn upper_bound(self) -> f32 {
        self.high
    }

    /// pbrt-v4 `Interval::LowerBound`.
    #[inline]
    #[must_use]
    pub fn lower_bound(self) -> f32 {
        self.low
    }

    /// Bound `i`: the lower for 0, the upper for 1.
    #[inline]
    fn bound(self, i: usize) -> f32 {
        [self.low, self.high][i]
    }

    /// The middle of the interval: the value pbrt-v4 converts it to.
    ///
    /// pbrt-v4 `Interval::Midpoint`.
    #[inline]
    #[must_use]
    #[expect(
        clippy::manual_midpoint,
        reason = "pbrt-v4's `(low + high) / 2`; `f32::midpoint` rounds differently"
    )]
    pub fn midpoint(self) -> f32 {
        (self.low + self.high) / 2.0
    }

    /// pbrt-v4 `Interval::Width`.
    #[inline]
    #[must_use]
    pub fn width(self) -> f32 {
        self.high - self.low
    }

    /// Whether the interval is exactly the single value `v`.
    ///
    /// pbrt-v4 `Interval::Exactly`.
    #[must_use]
    pub fn exactly(self, v: f32) -> bool {
        #[expect(clippy::float_cmp, reason = "an exact test by definition")]
        let exact = self.low == v && self.high == v;
        exact
    }

    /// Whether `v` lies in the interval.
    ///
    /// pbrt-v4 `InRange(Float, Interval)`.
    #[inline]
    #[must_use]
    pub fn contains(self, v: f32) -> bool {
        v >= self.low && v <= self.high
    }

    /// Whether the two intervals share a value.
    ///
    /// pbrt-v4 `InRange(Interval, Interval)`.
    #[inline]
    #[must_use]
    pub fn overlaps(self, other: Self) -> bool {
        self.low <= other.high && self.high >= other.low
    }

    /// `⌊low⌋`.
    ///
    /// pbrt-v4 `Floor(Interval)`.
    #[must_use]
    pub fn floor(self) -> f32 {
        floor(self.low)
    }

    /// `⌈high⌉`.
    ///
    /// pbrt-v4 `Ceil(Interval)`.
    #[must_use]
    pub fn ceil(self) -> f32 {
        ceil(self.high)
    }

    /// The square, which unlike `self * self` is never negative.
    ///
    /// pbrt-v4 `Sqr(Interval)`.
    #[inline]
    #[must_use]
    pub fn sqr(self) -> Self {
        kernels::sqr(self)
    }

    /// pbrt-v4 `Sqrt(Interval)`.
    #[inline]
    #[must_use]
    pub fn sqrt(self) -> Self {
        kernels::sqrt(self)
    }

    /// pbrt-v4 `Abs(Interval)`.
    #[must_use]
    pub fn abs(self) -> Self {
        if self.low >= 0.0 {
            self
        } else if self.high <= 0.0 {
            Self::new(-self.high, -self.low)
        } else {
            Self::new(0.0, max(-self.low, self.high))
        }
    }

    /// The product with `s`, a power of two, which is exact.
    ///
    /// pbrt-v4 `MulPow2`.
    #[must_use]
    pub fn mul_pow2(self, s: f32) -> Self {
        debug_assert!(
            is_power_of_2_magnitude(s.abs()),
            "mul_pow2: {s} is not a power of two"
        );
        Self::new(
            min(self.low * s, self.high * s),
            max(self.low * s, self.high * s),
        )
    }

    /// `a · b + c` with each bound computed by one rounded-out FMA.
    ///
    /// pbrt-v4 `FMA(Interval, Interval, Interval)`.
    #[inline]
    #[must_use]
    pub fn fma(a: Self, b: Self, c: Self) -> Self {
        kernels::fma(a, b, c)
    }

    /// `Σ tᵢ²`, never negative, folded with FMA from the last term to the
    /// first as pbrt-v4's variadic recursion does.
    ///
    /// pbrt-v4 `SumSquares`.
    #[must_use]
    pub fn sum_squares<const N: usize>(terms: [Self; N]) -> Self {
        const { assert!(N > 0, "sum_squares needs a term") };
        let Some((last, rest)) = terms.split_last() else {
            return Self::default();
        };
        rest.iter().rev().fold(last.sqr(), |ss, &i| {
            let ss = Self::fma(i, i, ss);
            Self::new(max(0.0, ss.low), ss.high)
        })
    }
}

/// Whether `a > 0` is an exact power of two, the check of `MulPow2`.
fn is_power_of_2_magnitude(a: f32) -> bool {
    a.is_normal() && significand(a) == 0
}

impl Products for Interval {
    /// Evaluates the scalar `difference_of_products` at the bound
    /// combinations that give the extreme products, then widens by two ULPs.
    #[inline]
    fn difference_of_products(a: Self, b: Self, c: Self, d: Self) -> Self {
        kernels::difference_of_products(a, b, c, d)
    }

    #[inline]
    fn sum_of_products(a: Self, b: Self, c: Self, d: Self) -> Self {
        Self::difference_of_products(a, b, -c, d)
    }
}

impl From<f32> for Interval {
    /// The exact interval `[v, v]`.
    #[inline]
    fn from(v: f32) -> Self {
        Self { low: v, high: v }
    }
}

impl From<Interval> for f32 {
    /// The midpoint, as pbrt-v4's `explicit operator Float`.
    #[inline]
    fn from(i: Interval) -> Self {
        i.midpoint()
    }
}

impl Neg for Interval {
    type Output = Self;
    #[inline]
    fn neg(self) -> Self {
        Self {
            low: -self.high,
            high: -self.low,
        }
    }
}

impl Add for Interval {
    type Output = Self;
    #[inline]
    fn add(self, i: Self) -> Self {
        kernels::add(self, i)
    }
}

impl Sub for Interval {
    type Output = Self;
    #[inline]
    fn sub(self, i: Self) -> Self {
        kernels::sub(self, i)
    }
}

impl Mul for Interval {
    type Output = Self;
    #[inline]
    fn mul(self, i: Self) -> Self {
        kernels::mul(self, i)
    }
}

impl Div for Interval {
    type Output = Self;
    /// Everything, `[−∞, ∞]`, when the divisor straddles zero.
    #[inline]
    fn div(self, i: Self) -> Self {
        if i.contains(0.0) {
            return Self::new(f32::NEG_INFINITY, f32::INFINITY);
        }
        kernels::div(self, i)
    }
}

impl Add<f32> for Interval {
    type Output = Self;
    #[inline]
    fn add(self, f: f32) -> Self {
        self + Self::from(f)
    }
}

impl Sub<f32> for Interval {
    type Output = Self;
    #[inline]
    fn sub(self, f: f32) -> Self {
        self - Self::from(f)
    }
}

impl Mul<f32> for Interval {
    type Output = Self;
    #[inline]
    fn mul(self, f: f32) -> Self {
        f * self
    }
}

impl Div<f32> for Interval {
    type Output = Self;
    /// Everything, `[−∞, ∞]`, for a zero divisor.
    #[inline]
    fn div(self, f: f32) -> Self {
        if f == 0.0 {
            return Self::new(f32::NEG_INFINITY, f32::INFINITY);
        }
        kernels::div_scalar(self, f)
    }
}

impl Add<Interval> for f32 {
    type Output = Interval;
    #[inline]
    fn add(self, i: Interval) -> Interval {
        Interval::from(self) + i
    }
}

impl Sub<Interval> for f32 {
    type Output = Interval;
    #[inline]
    fn sub(self, i: Interval) -> Interval {
        Interval::from(self) - i
    }
}

impl Mul<Interval> for f32 {
    type Output = Interval;
    #[inline]
    fn mul(self, i: Interval) -> Interval {
        kernels::scale(self, i)
    }
}

impl Div<Interval> for f32 {
    type Output = Interval;
    /// Everything, `[−∞, ∞]`, when the divisor straddles zero.
    #[inline]
    fn div(self, i: Interval) -> Interval {
        if i.contains(0.0) {
            return Interval::new(f32::NEG_INFINITY, f32::INFINITY);
        }
        kernels::scalar_div(self, i)
    }
}

macro_rules! impl_assign {
    ($($trait:ident $method:ident $op:tt),*) => {$(
        impl $trait for Interval {
            #[inline]
            fn $method(&mut self, i: Self) {
                *self = *self $op i;
            }
        }

        impl $trait<f32> for Interval {
            #[inline]
            fn $method(&mut self, f: f32) {
                *self = *self $op f;
            }
        }
    )*};
}
impl_assign!(AddAssign add_assign +, SubAssign sub_assign -, MulAssign mul_assign *, DivAssign div_assign /);

impl crate::math::Real for Interval {
    #[inline]
    fn sqr(self) -> Self {
        self.sqr()
    }

    #[inline]
    fn sqrt(self) -> Self {
        self.sqrt()
    }

    #[inline]
    fn abs(self) -> Self {
        self.abs()
    }

    #[inline]
    fn fma(a: Self, b: Self, c: Self) -> Self {
        Self::fma(a, b, c)
    }
}
