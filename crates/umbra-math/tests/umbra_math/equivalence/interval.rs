//! `Interval` arithmetic against `crate::reference::interval`.

use crate::reference::interval as reference;
use umbra_math::{Interval, difference_of_products, sum_of_products};

use super::{COUNT, SPECIAL_FINITE, finite_samples, intervals, same_interval};

/// Every pair of finite special values as intervals, then random ones.
fn pairs() -> Vec<(Interval, Interval)> {
    let mut all: Vec<(Interval, Interval)> = SPECIAL_FINITE
        .iter()
        .flat_map(|&a| SPECIAL_FINITE.iter().map(move |&b| Interval::new(a, b)))
        .flat_map(|i| {
            SPECIAL_FINITE
                .iter()
                .map(move |&c| (i, Interval::new(c, c + 1.0)))
        })
        .collect();
    let s = intervals(23, COUNT);
    all.extend(s.as_chunks::<2>().0.iter().map(|&[a, b]| (a, b)));
    all
}

fn check(name: &str, a: Interval, b: Interval, opt: Interval, reference: Interval) {
    assert!(
        same_interval(opt, reference),
        "{name}({a:?}, {b:?}): {opt:?} vs {reference:?}"
    );
}

#[test]
fn arithmetic_matches() {
    for (a, b) in pairs() {
        check("add", a, b, a + b, reference::add(a, b));
        check("sub", a, b, a - b, reference::sub(a, b));
        check("mul", a, b, a * b, reference::mul(a, b));
        check("div", a, b, a / b, reference::div(a, b));
    }
}

#[test]
fn fma_matches() {
    let s = intervals(29, COUNT);
    for &[a, b, c] in s.as_chunks::<3>().0 {
        let (opt, reference) = (Interval::fma(a, b, c), reference::fma(a, b, c));
        assert!(
            same_interval(opt, reference),
            "fma({a:?}, {b:?}, {c:?}): {opt:?} vs {reference:?}"
        );
    }
}

#[test]
fn scalar_operations_match() {
    let s = finite_samples(31, COUNT);
    for (&i, &f) in intervals(37, COUNT).iter().zip(&s) {
        let f_interval = Interval::from(f);
        check("scale", f_interval, i, f * i, reference::scale(f, i));
        check("mul_scalar", i, f_interval, i * f, reference::scale(f, i));
        check(
            "div_scalar",
            i,
            f_interval,
            i / f,
            reference::div_scalar(i, f),
        );
        check(
            "scalar_div",
            f_interval,
            i,
            f / i,
            reference::scalar_div(f, i),
        );
        check(
            "add_scalar",
            i,
            f_interval,
            i + f,
            reference::add(i, f_interval),
        );
        check(
            "sub_scalar",
            i,
            f_interval,
            i - f,
            reference::sub(i, f_interval),
        );
    }
}

#[test]
fn unary_operations_match() {
    for (a, _) in pairs() {
        check("sqr", a, a, a.sqr(), reference::sqr(a));
        check("sqrt", a, a, a.sqrt(), reference::sqrt(a));
    }
}

#[test]
fn from_value_and_error_matches() {
    let (v, e) = (finite_samples(41, COUNT / 2), finite_samples(43, COUNT / 2));
    for (&v, &e) in v.iter().zip(&e) {
        // An error is a magnitude: a negative one has no meaning.
        let e = e.abs();
        let (opt, reference) = (
            Interval::from_value_and_error(v, e),
            reference::from_value_and_error(v, e),
        );
        assert!(
            same_interval(opt, reference),
            "from_value_and_error({v}, {e}): {opt:?} vs {reference:?}"
        );
        let e = e * 1e-6;
        let (opt, reference) = (
            Interval::from_value_and_error(v, e),
            reference::from_value_and_error(v, e),
        );
        assert!(
            same_interval(opt, reference),
            "from_value_and_error({v}, {e}): {opt:?} vs {reference:?}"
        );
    }
}

#[test]
fn products_match() {
    let s = intervals(47, COUNT);
    for &[a, b, c, d] in s.as_chunks::<4>().0 {
        let (opt, reference) = (
            difference_of_products(a, b, c, d),
            reference::difference_of_products(a, b, c, d),
        );
        assert!(
            same_interval(opt, reference),
            "difference_of_products({a:?}, {b:?}, {c:?}, {d:?}): {opt:?} vs {reference:?}"
        );
        let (opt, reference) = (
            sum_of_products(a, b, c, d),
            reference::sum_of_products(a, b, c, d),
        );
        assert!(
            same_interval(opt, reference),
            "sum_of_products({a:?}, {b:?}, {c:?}, {d:?}): {opt:?} vs {reference:?}"
        );
    }
}

/// Infinite bounds, in the combinations whose result is not a NaN.
#[test]
fn infinite_bounds_match() {
    let inf = Interval::new(f32::INFINITY, f32::INFINITY);
    let neg_inf = Interval::new(f32::NEG_INFINITY, f32::NEG_INFINITY);
    let wide = Interval::new(f32::NEG_INFINITY, f32::INFINITY);
    let huge = Interval::new(f32::MAX, f32::MAX);
    let unit = Interval::new(1.0, 2.0);
    for (a, b) in [
        (unit, inf),
        (inf, unit),
        (unit, neg_inf),
        (unit, wide),
        (huge, huge),
        (huge, unit),
        (inf, inf),
        (neg_inf, neg_inf),
    ] {
        check("add", a, b, a + b, reference::add(a, b));
        check("mul", a, b, a * b, reference::mul(a, b));
        check("div", a, b, a / b, reference::div(a, b));
        check(
            "fma",
            a,
            b,
            Interval::fma(a, b, unit),
            reference::fma(a, b, unit),
        );
        check("sqr", a, b, a.sqr(), reference::sqr(a));
        check("sqrt", a, b, a.sqrt(), reference::sqrt(a));
    }
    check("sub", inf, unit, inf - unit, reference::sub(inf, unit));
    check("sub", unit, inf, unit - inf, reference::sub(unit, inf));
    check(
        "sub",
        huge,
        neg_inf,
        huge - neg_inf,
        reference::sub(huge, neg_inf),
    );
}
