//! The compensated products of `SquareMatrix` as [`Vec4`] kernels
//! (ADR 0007): one output row at a time, its columns in the lanes, so the
//! `InnerProduct` of every element of the row runs as one lane-wise chain of
//! `TwoProd` and `TwoSum`. Each lane performs the operations of
//! `tests/umbra_math/reference/square_matrix.rs` in the same order.

use crate::{Mask4, Vec4};

/// pbrt-v4 `TwoProd` per lane: the product and its exact rounding error.
#[inline]
fn two_prod(a: Vec4, b: Vec4) -> (Vec4, Vec4) {
    let ab = a * b;
    (ab, a.mul_add(b, -ab))
}

/// pbrt-v4 `TwoSum` per lane: the sum and its exact rounding error.
#[inline]
fn two_sum(a: Vec4, b: Vec4) -> (Vec4, Vec4) {
    let s = a + b;
    let delta = s - a;
    (s, (a - (s - delta)) + (b - delta))
}

/// pbrt-v4 `InnerProduct` per lane over the term pairs `(a[k], b[k])`,
/// combined from the last pair to the first as the variadic recursion does.
#[inline]
fn inner_product<const N: usize>(a: [Vec4; N], b: [Vec4; N]) -> Vec4 {
    const { assert!(N > 0, "inner_product needs a term") };
    let (mut v, mut err) = two_prod(a[N - 1], b[N - 1]);
    for k in (0..N - 1).rev() {
        let (ab, ab_err) = two_prod(a[k], b[k]);
        let (sum, sum_err) = two_sum(ab, v);
        v = sum;
        err = ab_err + (err + sum_err);
    }
    v + err
}

/// pbrt-v4 `operator*(SquareMatrix<3>, SquareMatrix<3>)`: row `i` of the
/// product is `Σₖ a[i][k] · b[k]`, each term a scalar times a row of `b`.
#[inline]
pub(super) fn mul3(a: &[[f32; 3]; 3], b: &[[f32; 3]; 3]) -> [[f32; 3]; 3] {
    let b = b.map(|[x, y, z]| Vec4::new(x, y, z, 0.0));
    a.map(|row| {
        let [x, y, z, _] = inner_product(row.map(Vec4::splat), b).to_array();
        [x, y, z]
    })
}

/// pbrt-v4 `operator*(SquareMatrix<4>, SquareMatrix<4>)`.
#[inline]
pub(super) fn mul4(a: &[[f32; 4]; 4], b: &[[f32; 4]; 4]) -> [[f32; 4]; 4] {
    let b = b.map(Vec4::from);
    a.map(|row| inner_product(row.map(Vec4::splat), b).to_array())
}

/// The lanes where the adjugate takes the second of two columns.
const ODD: Mask4 = Mask4::new([false, true, false, true]);

/// The sign bit of the odd lanes flipped.
#[inline]
fn negate_odd(v: Vec4) -> Vec4 {
    ODD.select(-v, v)
}

/// The sign bit of the even lanes flipped.
#[inline]
fn negate_even(v: Vec4) -> Vec4 {
    ODD.select(v, -v)
}

/// `[p0, q1, p2, q3]`: the two columns interleaved as the adjugate reads
/// them.
#[inline]
fn interleave(p: Vec4, q: Vec4) -> Vec4 {
    ODD.select(q, p)
}

/// `[c[i], c[j], s[i], s[j]]`: a minor of the bottom rows for the first two
/// columns of an adjugate row, of the top rows for the last two.
#[inline]
fn minors(c: &[f32; 6], s: &[f32; 6], i: usize, j: usize) -> Vec4 {
    Vec4::new(c[i], c[j], s[i], s[j])
}

/// pbrt-v4's scaled adjugate of a 4 × 4 matrix from its shared 2 × 2 minors
/// `s` (top rows) and `c` (bottom rows), a row at a time: each element is
/// `scale · InnerProduct` of three products of a matrix element and a minor,
/// with the matrix elements read down the columns in the row order
/// `1, 0, 3, 2` and alternating signs.
#[inline]
pub(super) fn scaled_adjugate4(
    m: &[[f32; 4]; 4],
    s: &[f32; 6],
    c: &[f32; 6],
    scale: f32,
) -> [[f32; 4]; 4] {
    // The columns of `m`, each with its rows in the order `1, 0, 3, 2`.
    let [p0, p1, p2, p3] = Vec4::transpose(m.map(Vec4::from)).map(Vec4::swap_pairs);
    let scale = Vec4::splat(scale);
    let row = |a: [Vec4; 3], x: [Vec4; 3]| (scale * inner_product(a, x)).to_array();
    [
        row(
            [negate_odd(p1), interleave(p3, p2), -interleave(p2, p3)],
            [minors(c, s, 5, 5), minors(c, s, 3, 4), minors(c, s, 4, 3)],
        ),
        row(
            [negate_even(p0), interleave(p2, p3), -interleave(p3, p2)],
            [minors(c, s, 5, 5), minors(c, s, 2, 1), minors(c, s, 1, 2)],
        ),
        row(
            [negate_odd(p0), interleave(p3, p1), -interleave(p1, p3)],
            [minors(c, s, 4, 4), minors(c, s, 0, 2), minors(c, s, 2, 0)],
        ),
        row(
            [negate_even(p0), interleave(p1, p2), -interleave(p2, p1)],
            [minors(c, s, 3, 3), minors(c, s, 1, 0), minors(c, s, 0, 1)],
        ),
    ]
}
