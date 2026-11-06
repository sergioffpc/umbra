//! `umbra_math::float` and the FMA helpers of `umbra_math::math` against
//! their references.

use crate::reference::{float as reference, math as reference_math};
use umbra_math::{
    add_round_down, add_round_up, ceil, difference_of_products, div_round_down, div_round_up,
    evaluate_polynomial, floor, fma, fma_round_down, fma_round_up, inner_product, mul_round_down,
    mul_round_up, next_float_down, next_float_up, round_up_pow2, sqrt, sqrt_round_down,
    sqrt_round_up, sub_round_down, sub_round_up, sum_of_products, two_prod,
};

use super::{COUNT, SPECIAL, STRIDE, same, samples};

/// Every triple of special values, then random ones.
fn triples() -> Vec<(f32, f32, f32)> {
    let specials = SPECIAL.iter().flat_map(|&a| {
        SPECIAL
            .iter()
            .flat_map(move |&b| SPECIAL.iter().map(move |&c| (a, b, c)))
    });
    let s = samples(11, COUNT + COUNT / 2);
    let random = s.as_chunks::<3>().0.iter().map(|&[a, b, c]| (a, b, c));
    specials.chain(random).collect()
}

#[test]
fn fma_matches_libm() {
    for (a, b, c) in triples() {
        let (opt, reference) = (fma(a, b, c), reference::fma(a, b, c));
        assert!(
            same(opt, reference),
            "fma({a}, {b}, {c}): {opt} vs {reference}"
        );
    }
}

#[test]
fn sqrt_floor_ceil_match_libm() {
    for (a, _, _) in triples() {
        assert!(same(sqrt(a), reference::sqrt(a)), "sqrt({a})");
        assert!(same(floor(a), reference::floor(a)), "floor({a})");
        assert!(same(ceil(a), reference::ceil(a)), "ceil({a})");
    }
}

/// Both steps of `v`, bit for bit, NaN included.
fn check_next_float(bits: u32) {
    let v = f32::from_bits(bits);
    assert_eq!(
        next_float_up(v).to_bits(),
        reference::next_float_up(v).to_bits(),
        "next_float_up({bits:#010x})"
    );
    assert_eq!(
        next_float_down(v).to_bits(),
        reference::next_float_down(v).to_bits(),
        "next_float_down({bits:#010x})"
    );
}

/// Every value whose bits are a multiple of 251, every special value, and
/// every value within a few ULPs of them.
#[test]
fn next_float_matches_bit_for_bit() {
    let near_special = SPECIAL
        .iter()
        .flat_map(|v| (0..8_u32).map(|d| v.to_bits().wrapping_add(d).wrapping_sub(4)));
    let strided = (0..u32::MAX / STRIDE).map(|i| i * STRIDE);
    strided.chain(near_special).for_each(check_next_float);
}

/// All 2³² values; several seconds, so run on demand:
/// `cargo nextest run -p umbra-math --run-ignored all -E 'test(exhaustive)'`.
#[test]
#[ignore = "exhaustive sweep of every f32; run on demand"]
fn next_float_matches_bit_for_bit_exhaustive() {
    (0..=u32::MAX).for_each(check_next_float);
}

/// The rounded sums and differences of `(a, b)`.
fn rounded_sums(a: f32, b: f32) -> [(&'static str, f32, f32); 4] {
    [
        (
            "add_round_up",
            add_round_up(a, b),
            reference::add_round_up(a, b),
        ),
        (
            "add_round_down",
            add_round_down(a, b),
            reference::add_round_down(a, b),
        ),
        (
            "sub_round_up",
            sub_round_up(a, b),
            reference::sub_round_up(a, b),
        ),
        (
            "sub_round_down",
            sub_round_down(a, b),
            reference::sub_round_down(a, b),
        ),
    ]
}

/// The rounded products and quotients of `(a, b)`.
fn rounded_products(a: f32, b: f32) -> [(&'static str, f32, f32); 4] {
    [
        (
            "mul_round_up",
            mul_round_up(a, b),
            reference::mul_round_up(a, b),
        ),
        (
            "mul_round_down",
            mul_round_down(a, b),
            reference::mul_round_down(a, b),
        ),
        (
            "div_round_up",
            div_round_up(a, b),
            reference::div_round_up(a, b),
        ),
        (
            "div_round_down",
            div_round_down(a, b),
            reference::div_round_down(a, b),
        ),
    ]
}

/// The rounded square roots of `a` and FMAs of `(a, b, c)`.
fn rounded_triples(a: f32, b: f32, c: f32) -> [(&'static str, f32, f32); 4] {
    [
        (
            "sqrt_round_up",
            sqrt_round_up(a),
            reference::sqrt_round_up(a),
        ),
        (
            "sqrt_round_down",
            sqrt_round_down(a),
            reference::sqrt_round_down(a),
        ),
        (
            "fma_round_up",
            fma_round_up(a, b, c),
            reference::fma_round_up(a, b, c),
        ),
        (
            "fma_round_down",
            fma_round_down(a, b, c),
            reference::fma_round_down(a, b, c),
        ),
    ]
}

/// The unrounded results, in the order of [`rounded_sums`],
/// [`rounded_products`] and [`rounded_triples`].
fn unrounded(a: f32, b: f32, c: f32) -> [f32; 12] {
    let (sum, difference, product, quotient) = (a + b, a - b, a * b, a / b);
    let (root, fused) = (reference::sqrt(a), reference::fma(a, b, c));
    [
        sum, sum, difference, difference, product, product, quotient, quotient, root, root, fused,
        fused,
    ]
}

/// Bit for bit wherever the unrounded result is a number. Stepping a NaN by
/// one ULP moves its payload, as pbrt-v4's `NextFloatUp` does, and a NaN
/// computed from other values has no fixed payload (Miri draws it at
/// random), so a NaN can step to an infinity in one run and not another.
#[test]
fn rounded_operations_match() {
    for (a, b, c) in triples() {
        let all = rounded_sums(a, b)
            .into_iter()
            .chain(rounded_products(a, b))
            .chain(rounded_triples(a, b, c))
            .zip(unrounded(a, b, c))
            .filter(|&(_, raw)| !raw.is_nan());
        for ((name, opt, reference), _) in all {
            assert!(
                same(opt, reference),
                "{name}({a}, {b}, {c}): {opt} vs {reference}"
            );
        }
    }
}

#[test]
fn products_match() {
    let s = samples(13, COUNT);
    for &[a, b, c, d] in s.as_chunks::<4>().0 {
        assert!(
            same(
                difference_of_products(a, b, c, d),
                reference_math::difference_of_products(a, b, c, d)
            ),
            "difference_of_products({a}, {b}, {c}, {d})"
        );
        assert!(
            same(
                sum_of_products(a, b, c, d),
                reference_math::sum_of_products(a, b, c, d)
            ),
            "sum_of_products({a}, {b}, {c}, {d})"
        );
        let (tp, rp) = (two_prod(a, b), reference_math::two_prod(a, b));
        assert!(
            same(tp.v, rp.v) && same(tp.err, rp.err),
            "two_prod({a}, {b})"
        );
        assert!(
            same(
                inner_product([(a, b), (c, d), (b, c)]),
                reference_math::inner_product([(a, b), (c, d), (b, c)])
            ),
            "inner_product({a}, {b}, {c}, {d})"
        );
        assert!(
            same(
                evaluate_polynomial(a, [b, c, d, 1.0]),
                reference_math::evaluate_polynomial(a, [b, c, d, 1.0])
            ),
            "evaluate_polynomial({a}, {b}, {c}, {d})"
        );
    }
}

/// Every `u32` that is a multiple of 251 and the edges, plus random `u64`s.
#[test]
fn round_up_pow2_matches() {
    let edges = [
        0_u32,
        1,
        2,
        3,
        u32::MAX,
        1 << 31,
        (1 << 31) - 1,
        (1 << 31) + 1,
    ];
    for v in (0..u32::MAX / STRIDE).map(|i| i * STRIDE).chain(edges) {
        assert_eq!(
            round_up_pow2(v),
            reference_math::round_up_pow2_u32(v),
            "{v}"
        );
        let i = v.cast_signed();
        assert_eq!(
            round_up_pow2(i),
            reference_math::round_up_pow2_u32(v).cast_signed(),
            "{i}"
        );
    }
    let mut rng = crate::rng::Rng::new(17);
    for _ in 0..COUNT / 2 {
        let v = (u64::from(rng.uniform_u32()) << 32) | u64::from(rng.uniform_u32());
        let v = v >> (rng.uniform_u32() % 64);
        assert_eq!(
            round_up_pow2(v),
            reference_math::round_up_pow2_u64(v),
            "{v}"
        );
        assert_eq!(
            round_up_pow2(v.cast_signed()),
            reference_math::round_up_pow2_u64(v).cast_signed(),
            "{v}"
        );
    }
}

/// All 2³² values of `round_up_pow2` on `u32`; run on demand.
#[test]
#[ignore = "exhaustive sweep of every u32; run on demand"]
fn round_up_pow2_matches_exhaustive() {
    for v in 0..=u32::MAX {
        assert_eq!(
            round_up_pow2(v),
            reference_math::round_up_pow2_u32(v),
            "{v}"
        );
    }
}
