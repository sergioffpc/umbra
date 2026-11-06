//! The reference of `umbra_math`'s `interval` module: pbrt-v4's `Interval` arithmetic, one
//! scalar rounded operation per bound, over [`crate::reference::float`].

use crate::reference::float::{
    add_round_down, add_round_up, div_round_down, div_round_up, fma_round_down, fma_round_up,
    mul_round_down, mul_round_up, next_float_down, next_float_up, sqrt_round_down, sqrt_round_up,
    sub_round_down, sub_round_up,
};
use crate::reference::math::difference_of_products as scalar_difference_of_products;
use umbra_math::{Interval, max, min};

/// The bounds `[low, high]` as an array, for the index arithmetic of
/// [`difference_of_products`].
fn bounds(i: Interval) -> [f32; 2] {
    [i.lower_bound(), i.upper_bound()]
}

/// pbrt-v4 `operator+(Interval, Interval)`.
#[must_use]
pub(crate) fn add(a: Interval, b: Interval) -> Interval {
    Interval::new(
        add_round_down(a.lower_bound(), b.lower_bound()),
        add_round_up(a.upper_bound(), b.upper_bound()),
    )
}

/// pbrt-v4 `operator-(Interval, Interval)`.
#[must_use]
pub(crate) fn sub(a: Interval, b: Interval) -> Interval {
    Interval::new(
        sub_round_down(a.lower_bound(), b.upper_bound()),
        sub_round_up(a.upper_bound(), b.lower_bound()),
    )
}

/// pbrt-v4 `operator*(Interval, Interval)`.
#[must_use]
pub(crate) fn mul(a: Interval, b: Interval) -> Interval {
    let lp = [
        mul_round_down(a.lower_bound(), b.lower_bound()),
        mul_round_down(a.upper_bound(), b.lower_bound()),
        mul_round_down(a.lower_bound(), b.upper_bound()),
        mul_round_down(a.upper_bound(), b.upper_bound()),
    ];
    let hp = [
        mul_round_up(a.lower_bound(), b.lower_bound()),
        mul_round_up(a.upper_bound(), b.lower_bound()),
        mul_round_up(a.lower_bound(), b.upper_bound()),
        mul_round_up(a.upper_bound(), b.upper_bound()),
    ];
    Interval::new(
        min(min(lp[0], lp[1]), min(lp[2], lp[3])),
        max(max(hp[0], hp[1]), max(hp[2], hp[3])),
    )
}

/// pbrt-v4 `operator/(Interval, Interval)`: everything when the divisor
/// straddles zero.
#[must_use]
pub(crate) fn div(a: Interval, b: Interval) -> Interval {
    if b.contains(0.0) {
        return Interval::new(f32::NEG_INFINITY, f32::INFINITY);
    }
    let lq = [
        div_round_down(a.lower_bound(), b.lower_bound()),
        div_round_down(a.upper_bound(), b.lower_bound()),
        div_round_down(a.lower_bound(), b.upper_bound()),
        div_round_down(a.upper_bound(), b.upper_bound()),
    ];
    let hq = [
        div_round_up(a.lower_bound(), b.lower_bound()),
        div_round_up(a.upper_bound(), b.lower_bound()),
        div_round_up(a.lower_bound(), b.upper_bound()),
        div_round_up(a.upper_bound(), b.upper_bound()),
    ];
    Interval::new(
        min(min(lq[0], lq[1]), min(lq[2], lq[3])),
        max(max(hq[0], hq[1]), max(hq[2], hq[3])),
    )
}

/// pbrt-v4 `FMA(Interval, Interval, Interval)`.
#[must_use]
pub(crate) fn fma(a: Interval, b: Interval, c: Interval) -> Interval {
    let (al, ah, bl, bh) = (
        a.lower_bound(),
        a.upper_bound(),
        b.lower_bound(),
        b.upper_bound(),
    );
    let (cl, ch) = (c.lower_bound(), c.upper_bound());
    let low = min(
        min(fma_round_down(al, bl, cl), fma_round_down(ah, bl, cl)),
        min(fma_round_down(al, bh, cl), fma_round_down(ah, bh, cl)),
    );
    let high = max(
        max(fma_round_up(al, bl, ch), fma_round_up(ah, bl, ch)),
        max(fma_round_up(al, bh, ch), fma_round_up(ah, bh, ch)),
    );
    Interval::new(low, high)
}

/// pbrt-v4 `operator*(Float, Interval)`.
#[must_use]
pub(crate) fn scale(s: f32, i: Interval) -> Interval {
    if s > 0.0 {
        Interval::new(
            mul_round_down(s, i.lower_bound()),
            mul_round_up(s, i.upper_bound()),
        )
    } else {
        Interval::new(
            mul_round_down(s, i.upper_bound()),
            mul_round_up(s, i.lower_bound()),
        )
    }
}

/// pbrt-v4 `operator/(Interval, Float)`: everything for a zero divisor.
#[must_use]
pub(crate) fn div_scalar(i: Interval, f: f32) -> Interval {
    if f == 0.0 {
        return Interval::new(f32::NEG_INFINITY, f32::INFINITY);
    }
    if f > 0.0 {
        Interval::new(
            div_round_down(i.lower_bound(), f),
            div_round_up(i.upper_bound(), f),
        )
    } else {
        Interval::new(
            div_round_down(i.upper_bound(), f),
            div_round_up(i.lower_bound(), f),
        )
    }
}

/// pbrt-v4 `operator/(Float, Interval)`: everything when the divisor
/// straddles zero.
#[must_use]
pub(crate) fn scalar_div(f: f32, i: Interval) -> Interval {
    if i.contains(0.0) {
        return Interval::new(f32::NEG_INFINITY, f32::INFINITY);
    }
    if f > 0.0 {
        Interval::new(
            div_round_down(f, i.upper_bound()),
            div_round_up(f, i.lower_bound()),
        )
    } else {
        Interval::new(
            div_round_down(f, i.lower_bound()),
            div_round_up(f, i.upper_bound()),
        )
    }
}

/// pbrt-v4 `Sqrt(Interval)`.
#[must_use]
pub(crate) fn sqrt(i: Interval) -> Interval {
    Interval::new(
        sqrt_round_down(i.lower_bound()),
        sqrt_round_up(i.upper_bound()),
    )
}

/// pbrt-v4 `Sqr(Interval)`.
#[must_use]
pub(crate) fn sqr(i: Interval) -> Interval {
    let (a, b) = (i.lower_bound().abs(), i.upper_bound().abs());
    let (abs_low, abs_high) = if a > b { (b, a) } else { (a, b) };
    if i.contains(0.0) {
        return Interval::new(0.0, mul_round_up(abs_high, abs_high));
    }
    Interval::new(
        mul_round_down(abs_low, abs_low),
        mul_round_up(abs_high, abs_high),
    )
}

/// pbrt-v4 `Interval::FromValueAndError` for an error `err ≥ 0`, for which
/// the bounds come out ordered, so `Interval::new` stores them as pbrt-v4's
/// member assignment does.
#[must_use]
pub(crate) fn from_value_and_error(v: f32, err: f32) -> Interval {
    debug_assert!(
        err >= 0.0 || err.is_nan(),
        "from_value_and_error: negative error {err}"
    );
    if err == 0.0 {
        Interval::from(v)
    } else {
        Interval::new(sub_round_down(v, err), add_round_up(v, err))
    }
}

/// The four products of the bounds of `a` and `b`, and the indices of the
/// smallest and largest of them.
fn bound_products(a: Interval, b: Interval) -> (usize, usize) {
    let p = [
        a.lower_bound() * b.lower_bound(),
        a.upper_bound() * b.lower_bound(),
        a.lower_bound() * b.upper_bound(),
        a.upper_bound() * b.upper_bound(),
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

/// pbrt-v4 `DifferenceOfProducts(Interval, …)`: the scalar
/// `difference_of_products` at the bound combinations that give the extreme
/// products, widened by two ULPs.
#[must_use]
pub(crate) fn difference_of_products(
    a: Interval,
    b: Interval,
    c: Interval,
    d: Interval,
) -> Interval {
    let (ab_low, ab_high) = bound_products(a, b);
    let (cd_low, cd_high) = bound_products(c, d);
    let (a, b, c, d) = (bounds(a), bounds(b), bounds(c), bounds(d));
    // `cd` is subtracted, so its extreme indices swap roles.
    let low = scalar_difference_of_products(
        a[ab_low & 1],
        b[ab_low >> 1],
        c[cd_high & 1],
        d[cd_high >> 1],
    );
    let high = scalar_difference_of_products(
        a[ab_high & 1],
        b[ab_high >> 1],
        c[cd_low & 1],
        d[cd_low >> 1],
    );
    debug_assert!(low <= high, "difference_of_products: {low} > {high}");
    Interval::new(
        next_float_down(next_float_down(low)),
        next_float_up(next_float_up(high)),
    )
}

/// pbrt-v4 `SumOfProducts(Interval, …)`.
#[must_use]
pub(crate) fn sum_of_products(a: Interval, b: Interval, c: Interval, d: Interval) -> Interval {
    difference_of_products(a, b, -c, d)
}
