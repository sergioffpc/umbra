//! The reference of `umbra_math`'s `math` module: the FMA-based accuracy
//! helpers over glibc's `fmaf`.

use crate::reference::float::fma;
use umbra_math::CompensatedFloat;

/// pbrt-v4 `DifferenceOfProducts` for `Float`.
#[must_use]
pub(crate) fn difference_of_products(a: f32, b: f32, c: f32, d: f32) -> f32 {
    let cd = c * d;
    let difference = fma(a, b, -cd);
    let error = fma(-c, d, cd);
    difference + error
}

/// pbrt-v4 `SumOfProducts` for `Float`.
#[must_use]
pub(crate) fn sum_of_products(a: f32, b: f32, c: f32, d: f32) -> f32 {
    let cd = c * d;
    let sum = fma(a, b, cd);
    let error = fma(c, d, -cd);
    sum + error
}

/// pbrt-v4 `TwoProd`.
#[must_use]
pub(crate) fn two_prod(a: f32, b: f32) -> CompensatedFloat {
    let ab = a * b;
    CompensatedFloat {
        v: ab,
        err: fma(a, b, -ab),
    }
}

/// pbrt-v4 `TwoSum`.
#[must_use]
pub(crate) fn two_sum(a: f32, b: f32) -> CompensatedFloat {
    let s = a + b;
    let delta = s - a;
    CompensatedFloat {
        v: s,
        err: (a - (s - delta)) + (b - delta),
    }
}

/// pbrt-v4 `InnerProduct`: the terms combined from the last pair to the
/// first, as the variadic recursion does.
#[must_use]
pub(crate) fn inner_product<const N: usize>(terms: [(f32, f32); N]) -> f32 {
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

/// pbrt-v4 `EvaluatePolynomial`, by Horner's rule with FMA.
#[must_use]
pub(crate) fn evaluate_polynomial<const N: usize>(t: f32, c: [f32; N]) -> f32 {
    const { assert!(N > 0, "evaluate_polynomial needs a coefficient") };
    c.iter()
        .rev()
        .copied()
        .reduce(|acc, ci| fma(t, acc, ci))
        .unwrap_or(0.0)
}

/// pbrt-v4 `RoundUpPow2` for 32-bit integers, by smearing the top bit down
/// in five shift-or steps.
#[must_use]
pub(crate) fn round_up_pow2_u32(v: u32) -> u32 {
    let mut v = v.wrapping_sub(1);
    let mut shift = 1;
    while shift < u32::BITS {
        v |= v >> shift;
        shift <<= 1;
    }
    v.wrapping_add(1)
}

/// pbrt-v4 `RoundUpPow2` for 64-bit integers.
#[must_use]
pub(crate) fn round_up_pow2_u64(v: u64) -> u64 {
    let mut v = v.wrapping_sub(1);
    let mut shift = 1;
    while shift < u64::BITS {
        v |= v >> shift;
        shift <<= 1;
    }
    v.wrapping_add(1)
}
