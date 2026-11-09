//! The `Vec4`/`Mask4` facade against its scalar reference (ADR 0007): every
//! lane of every operation is bit-identical to the scalar operation on that
//! lane, over random bit patterns (NaN, infinities, subnormals, signed zeros
//! included).
//!
//! Arithmetic that produces a NaN from non-NaN inputs gives the CPU's default
//! NaN, whose bits differ between CPU vendors; there both results only
//! have to be NaN. Everything that selects rather than computes (`min`,
//! `max`, `select`, `abs`, negation) is compared bit for bit, NaN included.

use crate::rng::Rng;
use umbra_math::{Mask4, Vec4, fma, max, min};

/// Special values every operation is tried on, against every other.
const SPECIAL: [f32; 12] = [
    0.0,
    -0.0,
    1.0,
    -1.5,
    f32::INFINITY,
    f32::NEG_INFINITY,
    f32::NAN,
    f32::MIN_POSITIVE,
    1e-40,
    f32::MAX,
    f32::from_bits(0x7fc0_1234),
    f32::from_bits(0xffc0_0001),
];

/// Lanes of random bits, with a special value in every third lane.
fn inputs(rng: &mut Rng) -> impl Iterator<Item = [f32; 4]> + '_ {
    let count = if cfg!(miri) { 2_000 } else { 20_000 };
    (0..count).map(move |i| core::array::from_fn(|lane| random_lane(rng, i + lane)))
}

/// A special value when `k` is a multiple of 3, otherwise random bits.
fn random_lane(rng: &mut Rng, k: usize) -> f32 {
    if k.is_multiple_of(3) {
        SPECIAL[rng.uniform_u32_below(12) as usize]
    } else {
        f32::from_bits(rng.uniform_u32())
    }
}

/// Bit-identical, or both NaN.
fn same_value(simd: f32, scalar: f32) -> bool {
    simd.to_bits() == scalar.to_bits() || (simd.is_nan() && scalar.is_nan())
}

/// Checks `simd` against `scalar` lane by lane on random `(a, b, c)` triples;
/// `bits` demands identical bits even for NaN.
fn check_ternary(
    simd: impl Fn(Vec4, Vec4, Vec4) -> Vec4,
    scalar: impl Fn(f32, f32, f32) -> f32,
    equal: impl Fn(f32, f32) -> bool,
) {
    let mut rng = Rng::new(7);
    let all: Vec<[f32; 4]> = inputs(&mut rng).collect();
    for w in all.windows(3) {
        let &[a, b, c] = w else {
            unreachable!("windows of 3")
        };
        let r = simd(Vec4::from(a), Vec4::from(b), Vec4::from(c)).to_array();
        for lane in 0..4 {
            let expected = scalar(a[lane], b[lane], c[lane]);
            assert!(
                equal(r[lane], expected),
                "{:?} {:?} {:?}: {} vs {expected}",
                a[lane],
                b[lane],
                c[lane],
                r[lane]
            );
        }
    }
}

fn bits_equal(a: f32, b: f32) -> bool {
    a.to_bits() == b.to_bits()
}

#[test]
fn arithmetic_matches_scalar() {
    check_ternary(|a, b, _| a + b, |a, b, _| a + b, same_value);
    check_ternary(|a, b, _| a - b, |a, b, _| a - b, same_value);
    check_ternary(|a, b, _| a * b, |a, b, _| a * b, same_value);
    check_ternary(|a, b, _| a / b, |a, b, _| a / b, same_value);
}

#[test]
fn scalar_operands_match_scalar() {
    let mut rng = Rng::new(11);
    for a in inputs(&mut rng) {
        let s = a[2];
        let (v, sv) = (Vec4::from(a), Vec4::splat(s));
        for lane in 0..4 {
            assert!(same_value((v * s)[lane], a[lane] * s), "{a:?} * {s}");
            assert!(same_value((v / s)[lane], a[lane] / s), "{a:?} / {s}");
            assert!(same_value((v * sv)[lane], a[lane] * s), "{a:?} * {s}");
        }
    }
}

#[test]
fn mul_add_is_fused() {
    check_ternary(Vec4::mul_add, fma, same_value);
}

#[test]
fn sqrt_matches_scalar() {
    check_ternary(|a, _, _| a.sqrt(), |a, _, _| libm::sqrtf(a), same_value);
}

#[test]
fn sign_operations_match_scalar_bits() {
    check_ternary(|a, _, _| -a, |a, _, _| -a, bits_equal);
    check_ternary(|a, _, _| a.abs(), |a, _, _| a.abs(), bits_equal);
}

/// `min`/`max` select with `std::min`/`std::max` semantics, the NaN rule of
/// pbrt-v4's ray–box slab test: a NaN in `b` yields `a`.
#[test]
fn min_max_select_like_std_min_max() {
    check_ternary(|a, b, _| a.min(b), |a, b, _| min(a, b), bits_equal);
    check_ternary(|a, b, _| a.max(b), |a, b, _| max(a, b), bits_equal);
    let (t0, t_near) = (Vec4::splat(0.5), Vec4::splat(f32::NAN));
    assert_eq!(0.5, t0.max(t_near)[0], "a NaN slab bound is ignored");
}

#[test]
fn comparisons_and_select_match_scalar() {
    type Cmp = (fn(Vec4, Vec4) -> Mask4, fn(&f32, &f32) -> bool);
    let cmps: [Cmp; 6] = [
        (Vec4::cmp_eq, f32::eq),
        (Vec4::cmp_ne, f32::ne),
        (Vec4::cmp_lt, f32::lt),
        (Vec4::cmp_le, f32::le),
        (Vec4::cmp_gt, f32::gt),
        (Vec4::cmp_ge, f32::ge),
    ];
    for (simd, scalar) in cmps {
        check_ternary(
            |a, b, c| simd(a, b).select(c, a),
            |a, b, c| if scalar(&a, &b) { c } else { a },
            bits_equal,
        );
    }
}

#[test]
fn mask_logic() {
    let a = Vec4::new(1.0, 2.0, f32::NAN, 4.0);
    let b = Vec4::splat(2.0);
    let lt = a.cmp_lt(b);
    assert_eq!([true, false, false, false], lt.to_array());
    assert!(lt.any() && !lt.all() && !lt.none());
    assert_eq!([false, true, true, true], (!lt).to_array());
    assert_eq!([true, true, false, false], (lt | a.cmp_eq(b)).to_array());
    assert_eq!([false, false, false, false], (lt & a.cmp_eq(b)).to_array());
    assert_eq!([true, true, false, true], (lt ^ a.cmp_ge(b)).to_array());
    assert_eq!(
        Mask4::from([true, false, true, false]).to_array(),
        [true, false, true, false]
    );
    assert!(Mask4::from([true; 4]).all() && Mask4::from([false; 4]).none());
}

/// Horizontal reductions fold the lanes left to right, as a scalar loop.
#[test]
fn reductions_fold_left_to_right() {
    let mut rng = Rng::new(13);
    for a in inputs(&mut rng) {
        let v = Vec4::from(a);
        assert!(
            same_value(v.reduce_add(), ((a[0] + a[1]) + a[2]) + a[3]),
            "{a:?}"
        );
        assert!(
            bits_equal(v.reduce_min(), min(min(min(a[0], a[1]), a[2]), a[3])),
            "{a:?}"
        );
        assert!(
            bits_equal(v.reduce_max(), max(max(max(a[0], a[1]), a[2]), a[3])),
            "{a:?}"
        );
    }
}

#[test]
fn construction_round_trips() {
    let a = [1.0, -2.0, f32::from_bits(0x7fc0_1234), -0.0];
    let v = Vec4::from(a);
    assert_eq!(a.map(f32::to_bits), v.to_array().map(f32::to_bits));
    assert_eq!(a.map(f32::to_bits), <[f32; 4]>::from(v).map(f32::to_bits));
    assert_eq!(
        Vec4::new(1.0, -2.0, 3.0, 4.0).to_array(),
        [1.0, -2.0, 3.0, 4.0]
    );
    assert_eq!(Vec4::splat(3.0).to_array(), [3.0; 4]);
    assert_eq!(Vec4::ZERO.to_array(), [0.0; 4]);
    assert_eq!(Vec4::new(1.0, 2.0, 3.0, 4.0), Vec4::new(1.0, 2.0, 3.0, 4.0));
}
