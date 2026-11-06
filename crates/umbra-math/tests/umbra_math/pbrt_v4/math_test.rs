//! pbrt-v4 `src/pbrt/util/math_test.cpp`, the scalar part. The `Interval`
//! tests of the same file are in `interval_test`.

use crate::rng::Rng;
use umbra_math::{
    CompensatedFloat, CompensatedSum, difference_of_products, erf_inv, evaluate_polynomial,
    fast_exp, find_interval, fma, gaussian, gaussian_integral, inner_product, is_power_of_2, lerp,
    log2_int, next_float_up, round_up_pow2, sum_of_products, two_prod, two_sum,
};

/// `TEST(Pow2, Basics)`.
#[test]
fn pow2_basics() {
    for i in 0..32 {
        let ui = 1_u32 << i;
        assert!(is_power_of_2(ui));
        if ui > 1 {
            assert!(!is_power_of_2(ui + 1));
        }
        if ui > 2 {
            assert!(!is_power_of_2(ui - 1));
        }
    }
}

/// `TEST(RoundUpPow2, Basics)`, for the `i32` overload.
#[test]
fn round_up_pow2_basics() {
    assert_eq!(round_up_pow2(7), 8);
    for i in 1..(1 << 24) {
        let expected = if is_power_of_2(i) {
            i
        } else {
            1 << (log2_int(i) + 1)
        };
        assert_eq!(round_up_pow2(i), expected);
    }
    for i in 0..30 {
        let v = 1 << i;
        assert_eq!(round_up_pow2(v), v);
        if v > 2 {
            assert_eq!(round_up_pow2(v - 1), v);
        }
        assert_eq!(round_up_pow2(v + 1), 2 * v);
    }
}

/// `TEST(Math, EvaluatePolynomial)`.
#[test]
fn math_evaluate_polynomial() {
    assert_eq!(4.0, evaluate_polynomial(100.0, [4.0]));
    assert_eq!(10.0, evaluate_polynomial(2.0, [4.0, 3.0]));
    assert_eq!(
        1.5 + 2.75 * 0.5 - 4.25 * 0.25 + 15.125 * 0.125,
        evaluate_polynomial(0.5, [1.5, 2.75, -4.25, 15.125])
    );
}

/// `TEST(Math, CompensatedSum)`, without the `long double` comparison.
#[test]
fn math_compensated_sum() {
    let mut kahan_sum_d = CompensatedSum::<f64>::default();
    let mut double_sum = 0.0_f64;
    let mut kahan_sum_f = CompensatedSum::<f32>::default();
    let mut float_sum = 0.0_f32;

    let mut rng = Rng::default();
    for _ in 0..16 * 1024 * 1024 {
        let v = libm::expf(lerp(rng.uniform_f32(), -5.0, 20.0));
        kahan_sum_d += f64::from(v);
        double_sum += f64::from(v);
        kahan_sum_f += v;
        float_sum += v;
    }

    let bits = |v: f64| v.to_bits().cast_signed();
    let kahan_d_bits = bits(kahan_sum_d.value());
    let double_error_ulps = (bits(double_sum) - kahan_d_bits).abs();
    let kahan_f_error_ulps = (bits(f64::from(kahan_sum_f.value())) - kahan_d_bits).abs();
    let float_error_ulps = (bits(f64::from(float_sum)) - kahan_d_bits).abs();

    assert!(double_error_ulps * 1000 < kahan_f_error_ulps);
    assert!(kahan_f_error_ulps * 10000 < float_error_ulps);
}

/// `TEST(Log2Int, Basics)`, the `u32` and `u64` overloads.
#[test]
fn log2_int_basics() {
    for i in 0..32 {
        let ui = 1_u32 << i;
        assert_eq!(i, log2_int(ui));
        assert_eq!(i, log2_int(u64::from(ui)));
    }
    for i in 1..31 {
        let ui = 1_u32 << i;
        assert_eq!(i, log2_int(ui + 1));
        assert_eq!(i, log2_int(u64::from(ui + 1)));
    }
}

/// `TEST(Log2Int, Basics)`, the `uint64_t` loops.
#[test]
fn log2_int_basics_u64() {
    for i in 0..64 {
        assert_eq!(i, log2_int(1_u64 << i));
    }
    for i in 1..64 {
        assert_eq!(i, log2_int((1_u64 << i) + 1));
    }
}

/// `TEST(Log2Int, Basics)`, the `float` overload: exact powers of two, and
/// rounding to the nearest exponent.
#[test]
fn log2_int_basics_float() {
    for i in -120_i16..=120 {
        assert_eq!(i32::from(i), log2_int(libm::exp2f(f32::from(i))));
    }
    for i in -31_i16..31 {
        let low = libm::powf(2.0, f32::from(i) + 0.499);
        assert_eq!(i32::from(i), log2_int(low));
        let high = libm::powf(2.0, f32::from(i) + 0.501);
        assert_eq!(i32::from(i) + 1, log2_int(high));
    }
}

/// `TEST(Math, ErfInv)`.
#[test]
fn math_erf_inv() {
    assert_eq!(0.0, erf_inv(libm::erff(0.0)));
    for x in [0.1125_f32, 0.25, 0.753, 1.521, 2.5115] {
        let e = libm::erff(x);
        let err = (erf_inv(e) - x).abs() / x;
        assert!(e >= 1.0 || err < 1e-4, "{x} erf {e} inv {}", erf_inv(e));
    }
}

fn random_magnitude(rng: &mut Rng) -> f32 {
    libm::powf(10.0, lerp(rng.uniform_f32(), -8.0, 8.0))
}

/// One ULP of `|v|`, as the pbrt-v4 tests measure it.
fn ulp(v: f32) -> f32 {
    next_float_up(v.abs()) - v.abs()
}

/// `TEST(Math, DifferenceOfProducts)`.
#[test]
fn math_difference_of_products() {
    for i in 0..100_000 {
        let mut rng = Rng::new(i);
        let (a, mut b, mut c, d) = (
            random_magnitude(&mut rng),
            random_magnitude(&mut rng),
            random_magnitude(&mut rng),
            random_magnitude(&mut rng),
        );
        // As in pbrt-v4, `< -0.5` never holds, so the sign is always 1.
        let sign = if rng.uniform_f32() < -0.5 { -1.0 } else { 1.0 };
        b *= sign;
        c *= sign;
        let dp = difference_of_products(a, b, c, d);
        #[expect(clippy::cast_possible_truncation, reason = "f64 reference to f32")]
        let dp2 = libm::fma(f64::from(a), f64::from(b), -f64::from(c) * f64::from(d)) as f32;
        assert!((dp - dp2).abs() < 2.0 * ulp(dp2), "{a} {b} {c} {d}");
    }
}

/// `TEST(Math, SumOfProducts)`.
#[test]
fn math_sum_of_products() {
    for i in 0..100_000 {
        let mut rng = Rng::new(i);
        let (a, mut b, mut c, d) = (
            random_magnitude(&mut rng),
            random_magnitude(&mut rng),
            random_magnitude(&mut rng),
            -random_magnitude(&mut rng),
        );
        let sign = if rng.uniform_f32() < -0.5 { -1.0 } else { 1.0 };
        b *= sign;
        c *= sign;
        let sp = sum_of_products(a, b, c, d);
        #[expect(clippy::cast_possible_truncation, reason = "f64 reference to f32")]
        let sp2 = libm::fma(f64::from(a), f64::from(b), f64::from(c) * f64::from(d)) as f32;
        assert!((sp - sp2).abs() < 2.0 * ulp(sp2), "{a} {b} {c} {d}");
    }
}

fn compare_floats(reference: f32, v: f32) -> bool {
    if reference.abs() < 1e-4 {
        (reference - v).abs() < 1e-5
    } else {
        ((reference - v) / reference).abs() < 1e-3
    }
}

/// `TEST(Math, GaussianIntegral)`.
#[test]
fn math_gaussian_integral() {
    for [mu, sigma] in [[0.0, 1.0], [0.0, 2.0], [0.0, 0.1], [1.0, 2.0], [-2.0, 1.0]] {
        let mut rng = Rng::default();
        for _ in 0..5 {
            let (x0, x1) = random_range(&mut rng);
            let est = estimate_gaussian_integral(&mut rng, x0, x1, mu, sigma);
            let integral = gaussian_integral(x0, x1, mu, sigma);
            assert!(compare_floats(est, integral), "{est} vs {integral}");
        }
    }
}

/// Two sorted values in `[5, 6)`, as pbrt-v4 draws them.
fn random_range(rng: &mut Rng) -> (f32, f32) {
    let x0 = -5.0 + 10.0 + rng.uniform_f32();
    let x1 = -5.0 + 10.0 + rng.uniform_f32();
    if x0 > x1 { (x1, x0) } else { (x0, x1) }
}

/// Stratified Monte Carlo estimate of the integral of `gaussian` over `[x0, x1]`.
fn estimate_gaussian_integral(rng: &mut Rng, x0: f32, x1: f32, mu: f32, sigma: f32) -> f32 {
    let n = 8192_u16;
    let sum: f32 = (0..n)
        .map(|k| {
            let u = (f32::from(k) + rng.uniform_f32()) / f32::from(n);
            gaussian(lerp(u, x0, x1), mu, sigma)
        })
        .sum();
    (x1 - x0) * sum / f32::from(n)
}

/// `TEST(FindInterval, Basics)`.
#[test]
fn find_interval_basics() {
    let a = [0.0_f32, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0];
    assert_eq!(0, find_interval(a.len(), |index| a[index] <= -1.0));
    assert_eq!(
        a.len() - 2,
        find_interval(a.len(), |index| a[index] <= 100.0)
    );
    for i in 0..9_u8 {
        let fi = f32::from(i);
        let i = usize::from(i);
        assert_eq!(i, find_interval(a.len(), |index| a[index] <= fi));
        assert_eq!(i, find_interval(a.len(), |index| a[index] <= fi + 0.5));
        if i > 0 {
            assert_eq!(i - 1, find_interval(a.len(), |index| a[index] <= fi - 0.5));
        }
    }
}

fn random_signed(rng: &mut Rng) -> f32 {
    let val = libm::powf(10.0, lerp(rng.uniform_f32(), -10.0, 10.0));
    let sign = if rng.uniform_f32() < 0.5 { -1.0 } else { 1.0 };
    val * sign
}

/// The value of `c` in `f64`, `v + err` without rounding to `f32` (pbrt-v4's
/// `explicit operator double`).
fn to_f64(c: CompensatedFloat) -> f64 {
    f64::from(c.v) + f64::from(c.err)
}

/// `TEST(Math, TwoProd)`.
#[test]
fn math_two_prod() {
    for i in 0..100_000 {
        let mut rng = Rng::new(i);
        let (a, b) = (random_signed(&mut rng), random_signed(&mut rng));
        let tp = two_prod(a, b);
        assert_eq!(f32::from(tp), a * b);
        assert_eq!(to_f64(tp), f64::from(a) * f64::from(b));
    }
}

/// `TEST(Math, TwoSum)`.
#[test]
fn math_two_sum() {
    for i in 0..100_000 {
        let mut rng = Rng::new(i);
        let (a, b) = (random_signed(&mut rng), random_signed(&mut rng));
        let ts = two_sum(a, b);
        assert_eq!(f32::from(ts), a + b);
        assert_eq!(to_f64(ts), f64::from(a) + f64::from(b));
    }
}

/// `TEST(Math, InnerProduct)`.
#[test]
fn math_inner_product() {
    for i in 0..100_000 {
        let mut rng = Rng::new(i);
        let a: [f32; 4] = core::array::from_fn(|_| random_signed(&mut rng));
        let b: [f32; 4] = core::array::from_fn(|_| random_signed(&mut rng));
        let ab = inner_product([(a[0], b[0]), (a[1], b[1]), (a[2], b[2]), (a[3], b[3])]);
        let dab: f64 = a
            .iter()
            .zip(&b)
            .map(|(&x, &y)| f64::from(x) * f64::from(y))
            .sum();
        #[expect(clippy::cast_possible_truncation, reason = "f64 reference to f32")]
        let dab = dab as f32;
        assert_eq!(ab, dab);
    }
}

/// `FMA` is the fused operation: one rounding, so `a·b − ab` recovers the
/// rounding error of `ab` exactly (the identity `TwoProd` relies on).
#[test]
fn fma_rounds_once() {
    let a = 1.0 + f32::EPSILON;
    let ab = a * a;
    assert_eq!(fma(a, a, -ab), f32::EPSILON * f32::EPSILON);
}

/// `TEST(FastExp, Accuracy)`.
#[test]
fn fast_exp_accuracy() {
    assert_eq!(1.0, fast_exp(0.0));
    let mut rng = Rng::new(6502);
    for _ in 0..100 {
        let v = lerp(rng.uniform_f32(), -20.0, 20.0);
        let (f, e) = (fast_exp(v), libm::expf(v));
        let err = ((f - e) / e).abs();
        assert!(
            err <= 0.0003,
            "at {v}: fast {f}, accurate {e}, relative error {err}"
        );
    }
}
