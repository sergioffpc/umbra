//! `Transform` application as [`Vec4`] kernels (ADR 0007): the four rows of
//! the matrix in the lanes, so one chain of multiplies and adds transforms a
//! point, and the error bound of a `Point3fi` comes out of the same lanes.
//! Each lane performs the operations of `tests/umbra_math/reference/transform.rs` in
//! the same order.
//!
//! The point kernels take the matrix by its [`columns`], which the
//! `Transform` computes once when it is built: transposing the rows on every
//! call cost as much as the arithmetic.

use crate::float::gamma;
use crate::interval::kernels::round_outward;
use crate::{Interval, Mask4, Normal3f, Point3, Point3f, Point3fi, Vec4, Vector3f, Vector3fi};

/// Column `j` of `m`: lane `i` is `m[i][j]`.
const fn column(m: &[[f32; 4]; 4], j: usize) -> Vec4 {
    Vec4::new(m[0][j], m[1][j], m[2][j], m[3][j])
}

/// The columns of `m`: lane `i` of column `j` is `m[i][j]`.
pub(super) const fn columns(m: &[[f32; 4]; 4]) -> [Vec4; 4] {
    [column(m, 0), column(m, 1), column(m, 2), column(m, 3)]
}

/// The rows `0, 1, 2` of `m` and a zero row.
#[inline]
fn upper_rows(m: &[[f32; 4]; 4]) -> [Vec4; 3] {
    [Vec4::from(m[0]), Vec4::from(m[1]), Vec4::from(m[2])]
}

/// `((c0·x + c1·y) + c2·z) + c3`: every row of `m` applied to `(x, y, z, 1)`
/// and summed left to right, as pbrt-v4's `operator()(Point3f)`.
#[inline]
fn rows_left_to_right(c: &[Vec4; 4], p: Point3f) -> Vec4 {
    ((c[0] * p.x + c[1] * p.y) + c[2] * p.z) + c[3]
}

/// `(c0·x + c1·y) + (c2·z + c3)`: every row of `m` applied to `(x, y, z, 1)`
/// and summed in pairs, as pbrt-v4's `ApplyInverse(Point3f)` and
/// `operator()(Point3fi)`.
#[inline]
fn rows_pairwise(c: &[Vec4; 4], p: Point3f) -> Vec4 {
    (c[0] * p.x + c[1] * p.y) + (c[2] * p.z + c[3])
}

/// `(c0·x + c1·y) + c2·z`: the upper 3 × 3 part applied to a vector.
#[inline]
fn rows_vector(c: &[Vec4; 4], v: Vector3f) -> Vec4 {
    (c[0] * v.x + c[1] * v.y) + c[2] * v.z
}

/// The first three lanes as a point.
#[inline]
fn point(v: Vec4) -> Point3f {
    Point3::new(v.lane::<0>(), v.lane::<1>(), v.lane::<2>())
}

/// `r / w` lane by lane, or `r` itself when `w` is exactly 1.
#[inline]
fn divide_by_w(r: Vec4) -> Vec4 {
    let w = r.lane::<3>();
    #[expect(
        clippy::float_cmp,
        reason = "pbrt-v4 skips the divide only for an exact 1"
    )]
    let affine = w == 1.0;
    if affine { r } else { r / w }
}

/// pbrt-v4 `Transform::operator()(Point3f)`, from the [`columns`] of the
/// matrix.
#[inline]
pub(super) fn point_left_to_right(col: &[Vec4; 4], p: Point3f) -> Point3f {
    point(divide_by_w(rows_left_to_right(col, p)))
}

/// pbrt-v4 `Transform::ApplyInverse(Point3f)`, from the [`columns`] of the
/// inverse.
#[inline]
pub(super) fn point_pairwise(col: &[Vec4; 4], p: Point3f) -> Point3f {
    let r = rows_pairwise(col, p);
    debug_assert!(r.lane::<3>() != 0.0, "apply_inverse: w = 0");
    point(divide_by_w(r))
}

/// pbrt-v4 `Transform::operator()(Vector3f)`, from the [`columns`] of the
/// matrix: each row of the upper 3 × 3 part summed left to right.
#[inline]
pub(super) fn vector(col: &[Vec4; 4], v: Vector3f) -> Vector3f {
    let r = rows_vector(col, v);
    Vector3f::new(r.lane::<0>(), r.lane::<1>(), r.lane::<2>())
}

/// `mᵀ` (the upper 3 × 3 part) times `(x, y, z)`, the rule for normals:
/// column `j` of the result is `(m[0][j]·x + m[1][j]·y) + m[2][j]·z`, so the
/// rows serve as the lanes directly.
#[inline]
pub(super) fn normal_transposed(m: &[[f32; 4]; 4], n: Normal3f) -> Normal3f {
    let [r0, r1, r2] = upper_rows(m);
    let v = (r0 * n.x + r1 * n.y) + r2 * n.z;
    Normal3f::new(v.lane::<0>(), v.lane::<1>(), v.lane::<2>())
}

/// Whether an error bound includes the translation column.
#[derive(Debug, Clone, Copy)]
enum Translation {
    /// Points move with the translation.
    Included,
    /// Vectors do not.
    Excluded,
}

/// The conservative error bound of the rows of `m` applied to the point or
/// vector with midpoint `c` and input error `e` (`None` when it is exact):
/// `γ₃` of the absolute products, plus `(1 + γ₃)` of the propagated input
/// error. Lane `i` is the bound of row `i`.
///
/// PBR 4e §6.8.5.
#[inline]
fn transformed_error(col: &[Vec4; 4], c: Vector3f, e: Option<Vector3f>, t: Translation) -> Vec4 {
    let products = ((col[0] * c.x).abs() + (col[1] * c.y).abs()) + (col[2] * c.z).abs();
    let rounding = gamma(3)
        * match t {
            Translation::Included => products + col[3].abs(),
            Translation::Excluded => products,
        };
    e.map_or(rounding, |e| {
        (gamma(3) + 1.0) * ((col[0].abs() * e.x + col[1].abs() * e.y) + col[2].abs() * e.z)
            + rounding
    })
}

/// The lanes `v ∓ e` with `e` in the odd lanes negated: `[v0 − e0, v1 + e1,
/// v2 − e2, v3 + e3]`.
#[inline]
fn minus_plus(v: Vec4, e: Vec4) -> Vec4 {
    const EVEN: Mask4 = Mask4::new([true, false, true, false]);
    v + EVEN.select(-e, e)
}

/// `Interval::from_value_and_error` for two values at once: lanes
/// `[v0, v0, v1, v1]` with errors `[e0, e0, e1, e1]` become the intervals
/// `[v0 − e0, v0 + e0]` and `[v1 − e1, v1 + e1]`, rounded outward, and
/// exactly `[v, v]` where the error is zero.
#[inline]
fn two_intervals(vv: Vec4, ee: Vec4) -> (Interval, Interval) {
    let rounded = round_outward(minus_plus(vv, ee));
    let r = ee.cmp_eq(Vec4::ZERO).select(vv, rounded);
    (
        Interval::from_bounds(r.lane::<0>(), r.lane::<1>()),
        Interval::from_bounds(r.lane::<2>(), r.lane::<3>()),
    )
}

/// The three intervals `[vᵢ − eᵢ, vᵢ + eᵢ]` of the first three lanes.
#[inline]
fn three_intervals(v: Vec4, e: Vec4) -> (Interval, Interval, Interval) {
    let (x, y) = two_intervals(v.unpack_lo(v), e.unpack_lo(e));
    let (z, _) = two_intervals(v.unpack_hi(v), e.unpack_hi(e));
    (x, y, z)
}

/// pbrt-v4 `Transform::operator()(Point3fi)`, from the [`columns`] of the
/// matrix: the rows summed in pairs, the error bound with the translation,
/// then the divide by `w`.
#[inline]
pub(super) fn point3fi(col: &[Vec4; 4], p: Point3fi) -> Point3fi {
    let c = Point3f::from(p);
    let r = rows_pairwise(col, c);
    let e = (!p.is_exact()).then(|| Vector3f::from(p.error()));
    let error = transformed_error(col, Vector3f::from(c), e, Translation::Included);
    let (x, y, z) = three_intervals(r, error);
    let pi = Point3fi::new(x, y, z);
    let w = r.lane::<3>();
    #[expect(
        clippy::float_cmp,
        reason = "pbrt-v4 skips the divide only for an exact 1"
    )]
    let affine = w == 1.0;
    if affine { pi } else { pi / w }
}

/// pbrt-v4 `Transform::operator()(Vector3fi)`, from the [`columns`] of the
/// matrix: the rows of the upper 3 × 3 part and the error bound without the
/// translation.
#[inline]
pub(super) fn vector3fi(col: &[Vec4; 4], v: Vector3fi) -> Vector3fi {
    let c = Vector3f::from(v);
    let e = (!v.is_exact()).then(|| v.error());
    let error = transformed_error(col, c, e, Translation::Excluded);
    let (x, y, z) = three_intervals(rows_vector(col, c), error);
    Vector3fi::new(x, y, z)
}
