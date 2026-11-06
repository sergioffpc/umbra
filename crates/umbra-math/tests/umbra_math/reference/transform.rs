//! The reference of `umbra_math`'s `transform` module: pbrt-v4's `Transform::operator()` and
//! `ApplyInverse` for points, vectors, normals and the error-bounded
//! `Point3fi`/`Vector3fi`, row by row in scalar code, with the interval
//! bounds from [`crate::reference::interval`].

use umbra_math::{Normal3f, Point3f, Point3fi, Transform, Vector3f, Vector3fi, gamma};

use crate::reference::interval::{div_scalar, from_value_and_error};

/// The rows of the forward matrix.
fn rows(t: &Transform) -> &[[f32; 4]; 4] {
    t.matrix().rows()
}

/// The rows of the inverse matrix.
fn inverse_rows(t: &Transform) -> &[[f32; 4]; 4] {
    t.inverse_matrix().rows()
}

/// `p / w`, or `p` itself when `w` is exactly 1.
fn divide_point(p: Point3f, w: f32) -> Point3f {
    #[expect(
        clippy::float_cmp,
        reason = "pbrt-v4 skips the divide only for an exact 1"
    )]
    let affine = w == 1.0;
    if affine { p } else { p / w }
}

/// pbrt-v4 `Transform::operator()(Point3f)`: each row summed left to right.
fn point_rows(m: &[[f32; 4]; 4], p: Point3f) -> Point3f {
    let row = |i: usize| m[i][0] * p.x + m[i][1] * p.y + m[i][2] * p.z + m[i][3];
    divide_point(Point3f::new(row(0), row(1), row(2)), row(3))
}

/// pbrt-v4 `Transform::ApplyInverse(Point3f)`: each row summed in pairs.
fn point_rows_pairwise(m: &[[f32; 4]; 4], p: Point3f) -> Point3f {
    let row = |i: usize| (m[i][0] * p.x + m[i][1] * p.y) + (m[i][2] * p.z + m[i][3]);
    let wp = row(3);
    debug_assert!(wp != 0.0, "apply_inverse: w = 0");
    divide_point(Point3f::new(row(0), row(1), row(2)), wp)
}

/// pbrt-v4 `Transform::operator()(Point3f)`.
#[must_use]
pub(crate) fn apply_point(t: &Transform, p: Point3f) -> Point3f {
    point_rows(rows(t), p)
}

/// pbrt-v4 `Transform::ApplyInverse(Point3f)`.
#[must_use]
pub(crate) fn apply_inverse_point(t: &Transform, p: Point3f) -> Point3f {
    point_rows_pairwise(inverse_rows(t), p)
}

/// `m` (the upper 3 × 3 part) times `(x, y, z)`.
fn mul_vector(m: &[[f32; 4]; 4], v: Vector3f) -> Vector3f {
    let row = |i: usize| m[i][0] * v.x + m[i][1] * v.y + m[i][2] * v.z;
    Vector3f::new(row(0), row(1), row(2))
}

/// `mᵀ` (the upper 3 × 3 part) times `(x, y, z)`, the rule for normals.
fn mul_transposed(m: &[[f32; 4]; 4], n: Normal3f) -> Normal3f {
    let col = |j: usize| m[0][j] * n.x + m[1][j] * n.y + m[2][j] * n.z;
    Normal3f::new(col(0), col(1), col(2))
}

/// pbrt-v4 `Transform::operator()(Vector3f)`.
#[must_use]
pub(crate) fn apply_vector(t: &Transform, v: Vector3f) -> Vector3f {
    mul_vector(rows(t), v)
}

/// pbrt-v4 `Transform::ApplyInverse(Vector3f)`.
#[must_use]
pub(crate) fn apply_inverse_vector(t: &Transform, v: Vector3f) -> Vector3f {
    mul_vector(inverse_rows(t), v)
}

/// pbrt-v4 `Transform::operator()(Normal3f)`.
#[must_use]
pub(crate) fn apply_normal(t: &Transform, n: Normal3f) -> Normal3f {
    mul_transposed(inverse_rows(t), n)
}

/// pbrt-v4 `Transform::ApplyInverse(Normal3f)`.
#[must_use]
pub(crate) fn apply_inverse_normal(t: &Transform, n: Normal3f) -> Normal3f {
    mul_transposed(rows(t), n)
}

/// Whether an error bound includes the translation column.
#[derive(Clone, Copy)]
enum Translation {
    Included,
    Excluded,
}

/// The conservative error bound of `m` (upper 3 × 4 part) applied to the
/// point or vector with midpoint `c` and input error `e` (`None` when it is
/// exact).
fn transformed_error(
    m: &[[f32; 4]; 4],
    c: Vector3f,
    e: Option<Vector3f>,
    t: Translation,
) -> Vector3f {
    let row = |r: &[f32; 4]| {
        let products = (r[0] * c.x).abs() + (r[1] * c.y).abs() + (r[2] * c.z).abs();
        let rounding = gamma(3)
            * match t {
                Translation::Included => products + r[3].abs(),
                Translation::Excluded => products,
            };
        e.map_or(rounding, |e| {
            (gamma(3) + 1.0) * (r[0].abs() * e.x + r[1].abs() * e.y + r[2].abs() * e.z) + rounding
        })
    };
    Vector3f::new(row(&m[0]), row(&m[1]), row(&m[2]))
}

/// Each component as the interval around its value with the given error.
fn point_with_error(p: Point3f, e: Vector3f) -> Point3fi {
    Point3fi::new(
        from_value_and_error(p.x, e.x),
        from_value_and_error(p.y, e.y),
        from_value_and_error(p.z, e.z),
    )
}

/// `m` applied to `p` with its error carried along.
fn point3fi_rows(m: &[[f32; 4]; 4], p: Point3fi) -> Point3fi {
    let c = Point3f::from(p);
    let row = |i: usize| (m[i][0] * c.x + m[i][1] * c.y) + (m[i][2] * c.z + m[i][3]);
    let e = (!p.is_exact()).then(|| Vector3f::from(p.error()));
    let error = transformed_error(m, Vector3f::from(c), e, Translation::Included);
    let pi = point_with_error(Point3f::new(row(0), row(1), row(2)), error);
    let w = row(3);
    #[expect(
        clippy::float_cmp,
        reason = "pbrt-v4 skips the divide only for an exact 1"
    )]
    let affine = w == 1.0;
    if affine {
        pi
    } else {
        Point3fi::new(
            div_scalar(pi.x, w),
            div_scalar(pi.y, w),
            div_scalar(pi.z, w),
        )
    }
}

/// pbrt-v4 `Transform::operator()(Point3fi)`.
#[must_use]
pub(crate) fn apply_point3fi(t: &Transform, p: Point3fi) -> Point3fi {
    point3fi_rows(rows(t), p)
}

/// Umbra's `Transform::apply_inverse(Point3fi)`: the forward rule with the
/// inverse matrix.
#[must_use]
pub(crate) fn apply_inverse_point3fi(t: &Transform, p: Point3fi) -> Point3fi {
    point3fi_rows(inverse_rows(t), p)
}

/// `m` applied to `v` with its error carried along.
fn vector3fi_rows(m: &[[f32; 4]; 4], v: Vector3fi) -> Vector3fi {
    let c = Vector3f::from(v);
    let e = (!v.is_exact()).then(|| v.error());
    let error = transformed_error(m, c, e, Translation::Excluded);
    let r = mul_vector(m, c);
    Vector3fi::new(
        from_value_and_error(r.x, error.x),
        from_value_and_error(r.y, error.y),
        from_value_and_error(r.z, error.z),
    )
}

/// pbrt-v4 `Transform::operator()(Vector3fi)`.
#[must_use]
pub(crate) fn apply_vector3fi(t: &Transform, v: Vector3fi) -> Vector3fi {
    vector3fi_rows(rows(t), v)
}

/// Umbra's `Transform::apply_inverse(Vector3fi)`.
#[must_use]
pub(crate) fn apply_inverse_vector3fi(t: &Transform, v: Vector3fi) -> Vector3fi {
    vector3fi_rows(inverse_rows(t), v)
}
