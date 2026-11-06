//! pbrt-v4 `src/pbrt/util/math_test.cpp`, the `FloatInterval` tests.

use crate::rng::Rng;
use umbra_math::{Interval, Products, difference_of_products, lerp, sum_of_products};

const FLOAT_INTERVAL_ITERS: u64 = 1_000_000;

/// pbrt-v4 `getFloat`: an exponentially distributed value with a random error
/// bound.
fn get_float(rng: &mut Rng) -> Interval {
    let val = libm::powf(10.0, lerp(rng.uniform_f32(), -6.0, 6.0));
    let ulp_error = |rng: &mut Rng, ulps: u32| {
        let offset = f32::from_bits(val.to_bits() + rng.uniform_u32_below(ulps));
        (offset - val).abs()
    };
    let err = match rng.uniform_u32_below(4) {
        0 => 0.0,
        1 => ulp_error(rng, 1024),
        2 => ulp_error(rng, 1024 * 1024),
        _ => (4.0 * rng.uniform_f32()) * val.abs(),
    };
    let sign = if rng.uniform_f32() < 0.5 { -1.0 } else { 1.0 };
    Interval::from_value_and_error(sign * val, err)
}

/// pbrt-v4 `getPrecise`: a value inside `ef`, at either end two times in
/// three.
fn get_precise(ef: Interval, rng: &mut Rng) -> f64 {
    match rng.uniform_u32_below(3) {
        0 => f64::from(ef.lower_bound()),
        1 => f64::from(ef.upper_bound()),
        _ => {
            let t = rng.uniform_f32();
            let p = (1.0 - t) * ef.lower_bound() + t * ef.upper_bound();
            f64::from(p.clamp(ef.lower_bound(), ef.upper_bound()))
        }
    }
}

/// `v` rounded to `f32`, as pbrt-v4 stores its precise results.
#[expect(clippy::cast_possible_truncation, reason = "the rounding is the point")]
fn to_f32(v: f64) -> f32 {
    v as f32
}

fn assert_contains(r: Interval, precise: f64, trial: u64) {
    assert!(
        f64::from(r.lower_bound()) <= precise,
        "{trial}: {precise} below {r:?}"
    );
    assert!(
        precise <= f64::from(r.upper_bound()),
        "{trial}: {precise} above {r:?}"
    );
}

/// Runs `check` on two random intervals and their precise values per trial.
fn binary_trials(check: impl Fn(Interval, Interval, f64, f64, u64)) {
    for trial in 0..FLOAT_INTERVAL_ITERS {
        let mut rng = Rng::new(trial);
        let ef = [get_float(&mut rng), get_float(&mut rng)];
        let precise = [get_precise(ef[0], &mut rng), get_precise(ef[1], &mut rng)];
        check(ef[0], ef[1], precise[0], precise[1], trial);
    }
}

/// `TEST(FloatInterval, Abs)`.
#[test]
fn float_interval_abs() {
    for trial in 0..FLOAT_INTERVAL_ITERS {
        let mut rng = Rng::new(trial);
        let ef = get_float(&mut rng);
        let precise = get_precise(ef, &mut rng);
        assert_contains(ef.abs(), precise.abs(), trial);
    }
}

/// `TEST(FloatInterval, Sqrt)`.
#[test]
fn float_interval_sqrt() {
    for trial in 0..FLOAT_INTERVAL_ITERS {
        let mut rng = Rng::new(trial);
        let ef = get_float(&mut rng);
        let precise = get_precise(ef, &mut rng);
        assert_contains(ef.abs().sqrt(), libm::sqrt(precise.abs()), trial);
    }
}

/// `TEST(FloatInterval, Add)`.
#[test]
fn float_interval_add() {
    binary_trials(|a, b, pa, pb, trial| {
        assert_contains(a + b, f64::from(to_f32(pa + pb)), trial);
    });
}

/// `TEST(FloatInterval, Sub)`.
#[test]
fn float_interval_sub() {
    binary_trials(|a, b, pa, pb, trial| {
        assert_contains(a - b, f64::from(to_f32(pa - pb)), trial);
    });
}

/// `TEST(FloatInterval, Mul)`.
#[test]
fn float_interval_mul() {
    binary_trials(|a, b, pa, pb, trial| {
        assert_contains(a * b, f64::from(to_f32(pa * pb)), trial);
    });
}

/// `TEST(FloatInterval, Div)`, skipping denominators that straddle zero.
#[test]
fn float_interval_div() {
    binary_trials(check_div);
}

fn check_div(a: Interval, b: Interval, pa: f64, pb: f64, trial: u64) {
    if b.lower_bound() * b.upper_bound() >= 0.0 {
        assert_contains(a / b, f64::from(to_f32(pa / pb)), trial);
    }
}

/// `TEST(FloatInterval, FMA)`, without its vacuous `nBetter > 0.85 · 0`.
#[test]
fn float_interval_fma() {
    for i in 0..10_000 {
        fma_chain(i);
    }
}

/// One trial: up to 400 chained `FMA`s, until a bound overflows.
fn fma_chain(trial: u64) {
    let mut rng = Rng::new(trial);
    let mut v = get_float(&mut rng).abs();
    for _ in 0..400 {
        let (a, b, c) = (v, get_float(&mut rng), get_float(&mut rng));
        v = Interval::fma(a, b, c);
        if v.lower_bound().is_infinite() || v.upper_bound().is_infinite() {
            return;
        }
        check_fma(&mut rng, [a, b, c], v, trial);
    }
}

/// `FMA(a, b, c)` holds the precise result and is no wider than `a * b + c`.
fn check_fma(rng: &mut Rng, [a, b, c]: [Interval; 3], v: Interval, trial: u64) {
    let (pa, pb, pc) = (
        get_precise(a, rng),
        get_precise(b, rng),
        get_precise(c, rng),
    );
    assert_contains(v, f64::from(to_f32(libm::fma(pa, pb, pc))), trial);
    let vp = a * b + c;
    assert!(v.lower_bound() >= vp.lower_bound(), "{v:?} vs {vp:?}");
    assert!(v.upper_bound() <= vp.upper_bound(), "{v:?} vs {vp:?}");
}

/// `TEST(FloatInterval, Sqr)`.
#[test]
fn float_interval_sqr() {
    let a = Interval::new(1.75, 2.25);
    let (as_, at) = (a.sqr(), a * a);
    assert_eq!(as_.upper_bound(), at.upper_bound());
    assert_eq!(as_.lower_bound(), at.lower_bound());

    // Straddles 0.
    let b = Interval::new(-0.75, 1.25);
    let (bs, b2) = (b.sqr(), b * b);
    assert_eq!(bs.upper_bound(), b2.upper_bound());
    assert_eq!(0.0, bs.lower_bound());
    assert!(b2.lower_bound() < 0.0);
}

/// `TEST(FloatInterval, SumSquares)`.
#[test]
fn float_interval_sum_squares() {
    let (a, b, c) = (
        Interval::from(1.0),
        Interval::from(2.0),
        Interval::from(3.0),
    );
    assert_eq!(1.0, f32::from(Interval::sum_squares([a])));
    assert_eq!(4.0, f32::from(Interval::sum_squares([b])));
    assert_eq!(5.0, f32::from(Interval::sum_squares([a, b])));
    assert_eq!(14.0, f32::from(Interval::sum_squares([a, b, c])));
}

/// Whether `product_terms` negates `d`, to mix signs in a sum of products.
#[derive(Debug, Clone, Copy)]
enum Last {
    Positive,
    Negated,
}

/// Four positive intervals, the middle two with a shared sign that, as in
/// pbrt-v4, is always 1; then the precise values inside them.
fn product_terms(rng: &mut Rng, last: Last) -> ([Interval; 4], [f64; 4]) {
    let a = get_float(rng).abs();
    let mut b = get_float(rng).abs();
    let mut c = get_float(rng).abs();
    let d = match last {
        Last::Positive => get_float(rng).abs(),
        Last::Negated => -get_float(rng).abs(),
    };
    let sign = if rng.uniform_f32() < -0.5 { -1.0 } else { 1.0 };
    b *= sign;
    c *= sign;
    let terms = [a, b, c, d];
    (terms, terms.map(|t| get_precise(t, rng)))
}

/// `TEST(FloatInterval, DifferenceOfProducts)`.
#[test]
fn float_interval_difference_of_products() {
    for trial in 0..FLOAT_INTERVAL_ITERS {
        let mut rng = Rng::new(trial);
        let ([a, b, c, d], [pa, pb, pc, pd]) = product_terms(&mut rng, Last::Positive);
        let r = difference_of_products(a, b, c, d);
        assert_contains(r, f64::difference_of_products(pa, pb, pc, pd), trial);
    }
}

/// `TEST(FloatInterval, SumOfProducts)`.
#[test]
fn float_interval_sum_of_products() {
    for trial in 0..FLOAT_INTERVAL_ITERS {
        let mut rng = Rng::new(trial);
        let ([a, b, c, d], [pa, pb, pc, pd]) = product_terms(&mut rng, Last::Negated);
        let r = sum_of_products(a, b, c, d);
        assert_contains(r, f64::sum_of_products(pa, pb, pc, pd), trial);
    }
}
