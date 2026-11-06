//! Interval arithmetic for conservative floating-point error bounds
//! (pbrt-v4 `Interval`, `src/pbrt/util/math.h`).

use core::ops::{Add, AddAssign, Div, DivAssign, Mul, MulAssign, Neg, Sub, SubAssign};

use crate::float::{
    add_round_down, add_round_up, div_round_down, div_round_up, fma_round_down, fma_round_up,
    mul_round_down, mul_round_up, next_float_down, next_float_up, significand, sqrt_round_down,
    sqrt_round_up, sub_round_down, sub_round_up,
};
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

    /// The interval `[v − err, v + err]`, rounded outward; exactly `[v, v]`
    /// when `err` is zero.
    ///
    /// pbrt-v4 `Interval::FromValueAndError`.
    #[must_use]
    pub fn from_value_and_error(v: f32, err: f32) -> Self {
        if err == 0.0 {
            Self { low: v, high: v }
        } else {
            Self {
                low: sub_round_down(v, err),
                high: add_round_up(v, err),
            }
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
        libm::floorf(self.low)
    }

    /// `⌈high⌉`.
    ///
    /// pbrt-v4 `Ceil(Interval)`.
    #[must_use]
    pub fn ceil(self) -> f32 {
        libm::ceilf(self.high)
    }

    /// The square, which unlike `self * self` is never negative.
    ///
    /// pbrt-v4 `Sqr(Interval)`.
    #[must_use]
    pub fn sqr(self) -> Self {
        let (a, b) = (self.low.abs(), self.high.abs());
        let (abs_low, abs_high) = if a > b { (b, a) } else { (a, b) };
        if self.contains(0.0) {
            return Self::new(0.0, mul_round_up(abs_high, abs_high));
        }
        Self::new(
            mul_round_down(abs_low, abs_low),
            mul_round_up(abs_high, abs_high),
        )
    }

    /// pbrt-v4 `Sqrt(Interval)`.
    #[must_use]
    pub fn sqrt(self) -> Self {
        Self::new(sqrt_round_down(self.low), sqrt_round_up(self.high))
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
    #[must_use]
    pub fn fma(a: Self, b: Self, c: Self) -> Self {
        let low = min(
            min(
                fma_round_down(a.low, b.low, c.low),
                fma_round_down(a.high, b.low, c.low),
            ),
            min(
                fma_round_down(a.low, b.high, c.low),
                fma_round_down(a.high, b.high, c.low),
            ),
        );
        let high = max(
            max(
                fma_round_up(a.low, b.low, c.high),
                fma_round_up(a.high, b.low, c.high),
            ),
            max(
                fma_round_up(a.low, b.high, c.high),
                fma_round_up(a.high, b.high, c.high),
            ),
        );
        Self::new(low, high)
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

    /// The bounds `[low, high]` as an array, for the index arithmetic of
    /// `difference_of_products`.
    fn bounds(self) -> [f32; 2] {
        [self.low, self.high]
    }
}

/// Whether `a > 0` is an exact power of two, the check of `MulPow2`.
fn is_power_of_2_magnitude(a: f32) -> bool {
    a.is_normal() && significand(a) == 0
}

/// The four products of the bounds of `a` and `b`, and the indices of the
/// smallest and largest of them.
fn bound_products(a: Interval, b: Interval) -> (usize, usize) {
    let p = [
        a.low * b.low,
        a.high * b.low,
        a.low * b.high,
        a.high * b.high,
    ];
    let low = min(min(p[0], p[1]), min(p[2], p[3]));
    let high = max(max(p[0], p[1]), max(p[2], p[3]));
    // The first equal product, as pbrt-v4's chain of `==` finds it (so -0
    // matches +0), and 3 when none is equal (NaN).
    #[expect(
        clippy::float_cmp,
        reason = "pbrt-v4 locates the extreme product by `==`"
    )]
    let index = |v: f32| p.iter().position(|&x| x == v).unwrap_or(3);
    (index(low), index(high))
}

impl Products for Interval {
    /// Evaluates the scalar `difference_of_products` at the bound
    /// combinations that give the extreme products, then widens by two ULPs.
    fn difference_of_products(a: Self, b: Self, c: Self, d: Self) -> Self {
        let (ab_low, ab_high) = bound_products(a, b);
        let (cd_low, cd_high) = bound_products(c, d);
        let (a, b, c, d) = (a.bounds(), b.bounds(), c.bounds(), d.bounds());
        // `cd` is subtracted, so its extreme indices swap roles.
        let low = f32::difference_of_products(
            a[ab_low & 1],
            b[ab_low >> 1],
            c[cd_high & 1],
            d[cd_high >> 1],
        );
        let high = f32::difference_of_products(
            a[ab_high & 1],
            b[ab_high >> 1],
            c[cd_low & 1],
            d[cd_low >> 1],
        );
        debug_assert!(low <= high, "difference_of_products: {low} > {high}");
        Self::new(
            next_float_down(next_float_down(low)),
            next_float_up(next_float_up(high)),
        )
    }

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
        Self::new(
            add_round_down(self.low, i.low),
            add_round_up(self.high, i.high),
        )
    }
}

impl Sub for Interval {
    type Output = Self;
    #[inline]
    fn sub(self, i: Self) -> Self {
        Self::new(
            sub_round_down(self.low, i.high),
            sub_round_up(self.high, i.low),
        )
    }
}

impl Mul for Interval {
    type Output = Self;
    fn mul(self, i: Self) -> Self {
        let (a, b) = (self, i);
        let lp = [
            mul_round_down(a.low, b.low),
            mul_round_down(a.high, b.low),
            mul_round_down(a.low, b.high),
            mul_round_down(a.high, b.high),
        ];
        let hp = [
            mul_round_up(a.low, b.low),
            mul_round_up(a.high, b.low),
            mul_round_up(a.low, b.high),
            mul_round_up(a.high, b.high),
        ];
        Self::new(
            min(min(lp[0], lp[1]), min(lp[2], lp[3])),
            max(max(hp[0], hp[1]), max(hp[2], hp[3])),
        )
    }
}

impl Div for Interval {
    type Output = Self;
    /// Everything, `[−∞, ∞]`, when the divisor straddles zero.
    fn div(self, i: Self) -> Self {
        if i.contains(0.0) {
            return Self::new(f32::NEG_INFINITY, f32::INFINITY);
        }
        let (a, b) = (self, i);
        let lq = [
            div_round_down(a.low, b.low),
            div_round_down(a.high, b.low),
            div_round_down(a.low, b.high),
            div_round_down(a.high, b.high),
        ];
        let hq = [
            div_round_up(a.low, b.low),
            div_round_up(a.high, b.low),
            div_round_up(a.low, b.high),
            div_round_up(a.high, b.high),
        ];
        Self::new(
            min(min(lq[0], lq[1]), min(lq[2], lq[3])),
            max(max(hq[0], hq[1]), max(hq[2], hq[3])),
        )
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
    fn div(self, f: f32) -> Self {
        if f == 0.0 {
            return Self::new(f32::NEG_INFINITY, f32::INFINITY);
        }
        if f > 0.0 {
            Self::new(div_round_down(self.low, f), div_round_up(self.high, f))
        } else {
            Self::new(div_round_down(self.high, f), div_round_up(self.low, f))
        }
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
    fn mul(self, i: Interval) -> Interval {
        if self > 0.0 {
            Interval::new(mul_round_down(self, i.low), mul_round_up(self, i.high))
        } else {
            Interval::new(mul_round_down(self, i.high), mul_round_up(self, i.low))
        }
    }
}

impl Div<Interval> for f32 {
    type Output = Interval;
    /// Everything, `[−∞, ∞]`, when the divisor straddles zero.
    fn div(self, i: Interval) -> Interval {
        if i.contains(0.0) {
            return Interval::new(f32::NEG_INFINITY, f32::INFINITY);
        }
        if self > 0.0 {
            Interval::new(div_round_down(self, i.high), div_round_up(self, i.low))
        } else {
            Interval::new(div_round_down(self, i.low), div_round_up(self, i.high))
        }
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
