//! The [`Interval`] operations as [`Vec4`] kernels (ADR 0007): the four
//! bound products in one multiply, the outward rounding of all of them in a
//! handful of selects, and the extremes by two deinterleaves. No branch
//! depends on the values.
//!
//! Each kernel performs the operations of its reference in
//! `tests/umbra_math/reference/interval.rs` on the same values in the same order, so
//! every bound is the same bits. The only reordering is across lanes, which
//! are independent.

use crate::Vec4;
use crate::interval::Interval;

/// The bit pattern of `+1` in every lane.
const PLUS_ONE: Vec4 = Vec4::from_bits([1; 4]);
/// The bit pattern of `−1` in every lane.
const MINUS_ONE: Vec4 = Vec4::from_bits([u32::MAX; 4]);

/// `next_float_up` per lane: the bit pattern of the lane (of `+0` for either
/// zero) one up where the lane is `≥ 0` and one down otherwise, except that
/// `+∞` stays.
#[inline]
pub(super) fn next_up(v: Vec4) -> Vec4 {
    let v0 = v.cmp_eq(Vec4::ZERO).select(Vec4::ZERO, v);
    let delta = v0.cmp_ge(Vec4::ZERO).select(PLUS_ONE, MINUS_ONE);
    let stepped = v0.add_i32(delta);
    v.cmp_eq(Vec4::splat(f32::INFINITY)).select(v, stepped)
}

/// `next_float_down` per lane: the bit pattern of the lane (of `−0` for
/// either zero) one down where the lane is `> 0` and one up otherwise, except
/// that `−∞` stays.
#[inline]
pub(super) fn next_down(v: Vec4) -> Vec4 {
    let v0 = v.cmp_eq(Vec4::ZERO).select(Vec4::splat(-0.0), v);
    let delta = v0.cmp_gt(Vec4::ZERO).select(MINUS_ONE, PLUS_ONE);
    let stepped = v0.add_i32(delta);
    v.cmp_eq(Vec4::splat(f32::NEG_INFINITY)).select(v, stepped)
}

/// Lanes 0 and 2 stepped down, lanes 1 and 3 stepped up: the outward
/// rounding of the bound pairs `[low, high, low, high]`, [`next_down`] and
/// [`next_up`] fused into one pass.
#[inline]
pub(crate) fn round_outward(v: Vec4) -> Vec4 {
    const ZEROS: Vec4 = Vec4::new(-0.0, 0.0, -0.0, 0.0);
    const INFINITIES: Vec4 = Vec4::new(
        f32::NEG_INFINITY,
        f32::INFINITY,
        f32::NEG_INFINITY,
        f32::INFINITY,
    );
    const UP: crate::Mask4 = crate::Mask4::new([false, true, false, true]);
    const OUT: Vec4 = Vec4::from_bits([1, u32::MAX, 1, u32::MAX]);
    const IN: Vec4 = Vec4::from_bits([u32::MAX, 1, u32::MAX, 1]);
    let v0 = v.cmp_eq(Vec4::ZERO).select(ZEROS, v);
    let towards_zero = UP.select_mask(v0.cmp_ge(Vec4::ZERO), v0.cmp_gt(Vec4::ZERO));
    let stepped = v0.add_i32(towards_zero.select(IN, OUT));
    v.cmp_eq(INFINITIES).select(v, stepped)
}

/// `[a.low, a.high, a.low, a.high]`.
#[inline]
fn low_high_twice(a: Interval) -> Vec4 {
    Vec4::new(a.low, a.high, a.low, a.high)
}

/// `[b.low, b.low, b.high, b.high]`.
#[inline]
fn low_low_high_high(b: Interval) -> Vec4 {
    Vec4::new(b.low, b.low, b.high, b.high)
}

/// `[low, high, 0, 0]`.
#[inline]
fn bounds(a: Interval) -> Vec4 {
    Vec4::new(a.low, a.high, 0.0, 0.0)
}

/// `[a.low, a.high]` for a positive `s`, else `[a.high, a.low]`: the bound
/// that gives the lower result when multiplied or divided by `s` first.
#[inline]
fn bounds_ordered_by(a: Interval, s: f32) -> Vec4 {
    if s > 0.0 {
        Vec4::new(a.low, a.high, 0.0, 0.0)
    } else {
        Vec4::new(a.high, a.low, 0.0, 0.0)
    }
}

/// The interval `[r0, r1]` of an outward-rounded pair, through
/// [`Interval::new`].
#[inline]
fn pair(r: Vec4) -> Interval {
    Interval::new(r.lane::<0>(), r.lane::<1>())
}

/// `min(min(l0, l1), min(l2, l3))` and `max(max(h0, h1), max(h2, h3))`, with
/// [`crate::min`]/[`crate::max`] semantics, by deinterleaving.
#[inline]
fn extremes(lo: Vec4, hi: Vec4) -> (f32, f32) {
    let (x, y) = (lo.unzip_lo(hi), lo.unzip_hi(hi));
    let (mn, mx) = (x.min(y), x.max(y));
    let (x, y) = (mn.unzip_lo(mx), mn.unzip_hi(mx));
    (x.min(y).lane::<0>(), x.max(y).lane::<3>())
}

/// The interval between the extremes of the four bound combinations.
#[inline]
fn hull(lo: Vec4, hi: Vec4) -> Interval {
    let (low, high) = extremes(lo, hi);
    Interval::new(low, high)
}

/// pbrt-v4 `operator+(Interval, Interval)`.
#[inline]
pub(super) fn add(a: Interval, b: Interval) -> Interval {
    pair(round_outward(bounds(a) + bounds(b)))
}

/// pbrt-v4 `operator-(Interval, Interval)`: `a.low − b.high` and
/// `a.high − b.low`, rounded outward.
#[inline]
pub(super) fn sub(a: Interval, b: Interval) -> Interval {
    pair(round_outward(
        bounds(a) - Vec4::new(b.high, b.low, 0.0, 0.0),
    ))
}

/// pbrt-v4 `operator*(Interval, Interval)`.
#[inline]
pub(super) fn mul(a: Interval, b: Interval) -> Interval {
    let p = low_high_twice(a) * low_low_high_high(b);
    hull(next_down(p), next_up(p))
}

/// pbrt-v4 `operator/(Interval, Interval)` for a divisor that does not
/// contain zero.
#[inline]
pub(super) fn div(a: Interval, b: Interval) -> Interval {
    let q = low_high_twice(a) / low_low_high_high(b);
    hull(next_down(q), next_up(q))
}

/// pbrt-v4 `FMA(Interval, Interval, Interval)`.
#[inline]
pub(super) fn fma(a: Interval, b: Interval, c: Interval) -> Interval {
    let (a, b) = (low_high_twice(a), low_low_high_high(b));
    let low = a.mul_add(b, Vec4::splat(c.low));
    let high = a.mul_add(b, Vec4::splat(c.high));
    hull(next_down(low), next_up(high))
}

/// pbrt-v4 `operator*(Float, Interval)`.
#[inline]
pub(super) fn scale(s: f32, a: Interval) -> Interval {
    pair(round_outward(Vec4::splat(s) * bounds_ordered_by(a, s)))
}

/// pbrt-v4 `operator/(Interval, Float)` for a non-zero divisor.
#[inline]
pub(super) fn div_scalar(a: Interval, d: f32) -> Interval {
    pair(round_outward(bounds_ordered_by(a, d) / Vec4::splat(d)))
}

/// pbrt-v4 `operator/(Float, Interval)` for a divisor that does not contain
/// zero: dividing a positive `n` by the upper bound gives the lower result,
/// so the bounds are ordered as for a scalar of the opposite sign.
#[inline]
pub(super) fn scalar_div(n: f32, a: Interval) -> Interval {
    pair(round_outward(Vec4::splat(n) / bounds_ordered_by(a, -n)))
}

/// pbrt-v4 `Sqrt(Interval)`.
#[inline]
pub(super) fn sqrt(a: Interval) -> Interval {
    let r = round_outward(bounds(a).sqrt());
    Interval::new(crate::max(0.0, r.lane::<0>()), r.lane::<1>())
}

/// pbrt-v4 `Sqr(Interval)`: the squares of the smaller and the larger
/// magnitude, rounded outward; from zero when the interval contains it.
#[inline]
pub(super) fn sqr(a: Interval) -> Interval {
    let (x, y) = (a.low.abs(), a.high.abs());
    let (abs_low, abs_high) = if x > y { (y, x) } else { (x, y) };
    let m = Vec4::new(abs_low, abs_high, 0.0, 0.0);
    let r = round_outward(m * m);
    let low = if a.contains(0.0) { 0.0 } else { r.lane::<0>() };
    Interval::new(low, r.lane::<1>())
}

/// pbrt-v4 `Interval::FromValueAndError` for a non-zero error: the bounds are
/// stored as computed, not sorted.
#[inline]
pub(super) fn from_value_and_error(v: f32, err: f32) -> Interval {
    let r = round_outward(Vec4::splat(v) + Vec4::new(-err, err, 0.0, 0.0));
    Interval {
        low: r.lane::<0>(),
        high: r.lane::<1>(),
    }
}

/// The indices, in `[a.low · b.low, a.high · b.low, a.low · b.high,
/// a.high · b.high]`, of the smallest and the largest product: the first
/// equal one, as pbrt-v4's chain of `==` finds it, and 3 when none is equal
/// (NaN).
#[inline]
fn extreme_products(a: Interval, b: Interval) -> (usize, usize) {
    let p = low_high_twice(a) * low_low_high_high(b);
    let (low, high) = extremes(p, p);
    let first = |v: f32| {
        let equal = p.cmp_eq(Vec4::splat(v)).bitmask();
        if equal == 0 {
            3
        } else {
            equal.trailing_zeros() as usize
        }
    };
    (first(low), first(high))
}

/// pbrt-v4 `DifferenceOfProducts(Interval, …)`: the scalar
/// `difference_of_products` at the bound combinations that give the extreme
/// products, computed for both bounds at once, then widened by two ULPs.
#[inline]
pub(super) fn difference_of_products(
    a: Interval,
    b: Interval,
    c: Interval,
    d: Interval,
) -> Interval {
    let (ab_low, ab_high) = extreme_products(a, b);
    let (cd_low, cd_high) = extreme_products(c, d);
    // `cd` is subtracted, so its extreme indices swap roles.
    let av = Vec4::new(a.bound(ab_low & 1), a.bound(ab_high & 1), 0.0, 0.0);
    let bv = Vec4::new(b.bound(ab_low >> 1), b.bound(ab_high >> 1), 0.0, 0.0);
    let cv = Vec4::new(c.bound(cd_high & 1), c.bound(cd_low & 1), 0.0, 0.0);
    let dv = Vec4::new(d.bound(cd_high >> 1), d.bound(cd_low >> 1), 0.0, 0.0);
    let cd = cv * dv;
    let difference = av.mul_add(bv, -cd);
    let error = (-cv).mul_add(dv, cd);
    let r = difference + error;
    debug_assert!(
        r.lane::<0>() <= r.lane::<1>(),
        "difference_of_products: {} > {}",
        r.lane::<0>(),
        r.lane::<1>()
    );
    pair(round_outward(round_outward(r)))
}
