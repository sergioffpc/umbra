//! Affine and projective transformations (pbrt-v4 `Transform`,
//! `src/pbrt/util/transform.h`). Transforming a `Ray` or `Bounds3f` belongs
//! with those types, in `umbra-geometry`.

use core::ops::Mul;

use crate::float::gamma;
use crate::math::radians;
use crate::square_matrix::SquareMatrix;
use crate::vecmath::{
    Frame, Normal3, Normal3f, Point3, Point3f, Point3fi, Vector3, Vector3f, Vector3fi, cross, dot,
};

/// A transformation and its inverse, as 4 × 4 matrices.
///
/// PBR 4e §3.9, pbrt-v4 `Transform`.
#[derive(Debug, Clone, Copy, Default)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[cfg_attr(
    feature = "serde",
    serde(from = "TransformData", into = "TransformData")
)]
pub struct Transform {
    m: SquareMatrix<4>,
    m_inv: SquareMatrix<4>,
}

/// The serialised form of a [`Transform`]: both matrices, by rows.
#[cfg(feature = "serde")]
#[derive(serde::Serialize, serde::Deserialize)]
struct TransformData {
    m: [[[f32; 4]; 4]; 2],
}

#[cfg(feature = "serde")]
impl From<TransformData> for Transform {
    fn from(d: TransformData) -> Self {
        Self::new(SquareMatrix::new(d.m[0]), SquareMatrix::new(d.m[1]))
    }
}

#[cfg(feature = "serde")]
impl From<Transform> for TransformData {
    fn from(t: Transform) -> Self {
        Self {
            m: [*t.m.rows(), *t.m_inv.rows()],
        }
    }
}

impl PartialEq for Transform {
    /// Compares the matrices only, as pbrt-v4 does.
    fn eq(&self, other: &Self) -> bool {
        self.m == other.m
    }
}

impl From<SquareMatrix<4>> for Transform {
    /// The transform of `m`, whose inverse is computed; for a singular `m`
    /// the inverse is all NaN, as in pbrt-v4.
    fn from(m: SquareMatrix<4>) -> Self {
        let m_inv = m
            .inverse()
            .unwrap_or_else(|| SquareMatrix::new([[f32::NAN; 4]; 4]));
        Self { m, m_inv }
    }
}

impl From<Frame> for Transform {
    /// The rotation into the frame's coordinates.
    ///
    /// pbrt-v4 `Transform(const Frame &)`.
    fn from(f: Frame) -> Self {
        Self::from(SquareMatrix::new([
            [f.x.x, f.x.y, f.x.z, 0.0],
            [f.y.x, f.y.y, f.y.z, 0.0],
            [f.z.x, f.z.y, f.z.z, 0.0],
            [0.0, 0.0, 0.0, 1.0],
        ]))
    }
}

impl Mul for Transform {
    type Output = Self;
    /// The composition: `self` applied after `t2`.
    ///
    /// pbrt-v4 `Transform::operator*`.
    fn mul(self, t2: Self) -> Self {
        Self {
            m: self.m * t2.m,
            m_inv: t2.m_inv * self.m_inv,
        }
    }
}

impl Transform {
    /// The transform with matrix `m` and the given inverse.
    #[must_use]
    pub const fn new(m: SquareMatrix<4>, m_inv: SquareMatrix<4>) -> Self {
        Self { m, m_inv }
    }

    /// pbrt-v4 `Transform::GetMatrix`.
    #[must_use]
    pub const fn matrix(&self) -> &SquareMatrix<4> {
        &self.m
    }

    /// pbrt-v4 `Transform::GetInverseMatrix`.
    #[must_use]
    pub const fn inverse_matrix(&self) -> &SquareMatrix<4> {
        &self.m_inv
    }

    /// pbrt-v4 `Inverse(Transform)`.
    #[must_use]
    pub const fn inverse(&self) -> Self {
        Self {
            m: self.m_inv,
            m_inv: self.m,
        }
    }

    /// pbrt-v4 `Transpose(Transform)`.
    #[must_use]
    pub fn transpose(&self) -> Self {
        Self {
            m: self.m.transpose(),
            m_inv: self.m_inv.transpose(),
        }
    }

    /// pbrt-v4 `Transform::IsIdentity`.
    #[must_use]
    pub fn is_identity(&self) -> bool {
        self.m.is_identity()
    }

    /// Whether the transform scales any coordinate axis by more than
    /// `tolerance` (pbrt-v4's default is `1e-3`).
    ///
    /// pbrt-v4 `Transform::HasScale`.
    #[must_use]
    pub fn has_scale(&self, tolerance: f32) -> bool {
        let axes = [
            Vector3f::new(1.0, 0.0, 0.0),
            Vector3f::new(0.0, 1.0, 0.0),
            Vector3f::new(0.0, 0.0, 1.0),
        ];
        axes.iter()
            .any(|&a| (self.apply(a).length_squared() - 1.0).abs() > tolerance)
    }

    /// Whether the transform turns a right-handed coordinate system into a
    /// left-handed one.
    ///
    /// pbrt-v4 `Transform::SwapsHandedness`.
    #[must_use]
    pub fn swaps_handedness(&self) -> bool {
        let m = self.m.rows();
        let upper = SquareMatrix::new([
            [m[0][0], m[0][1], m[0][2]],
            [m[1][0], m[1][1], m[1][2]],
            [m[2][0], m[2][1], m[2][2]],
        ]);
        upper.determinant() < 0.0
    }

    /// `x` transformed.
    ///
    /// pbrt-v4 `Transform::operator()`.
    #[must_use]
    pub fn apply<T: Transformable>(&self, x: T) -> T {
        x.transformed(self)
    }

    /// `x` transformed by the inverse.
    ///
    /// pbrt-v4 `Transform::ApplyInverse`.
    #[must_use]
    pub fn apply_inverse<T: Transformable>(&self, x: T) -> T {
        x.inverse_transformed(self)
    }
}

/// What a [`Transform`] applies to, each by its own rule: points take the
/// translation and the projective divide, vectors do not, and normals take
/// the inverse transpose.
pub trait Transformable: Copy {
    /// pbrt-v4 `Transform::operator()`.
    #[must_use]
    fn transformed(self, t: &Transform) -> Self;
    /// pbrt-v4 `Transform::ApplyInverse`.
    #[must_use]
    fn inverse_transformed(self, t: &Transform) -> Self;
}

impl Transformable for Point3f {
    /// Sums each row left to right; divides by `w` unless it is exactly 1.
    fn transformed(self, t: &Transform) -> Self {
        let (m, Point3 { x, y, z }) = (t.m.rows(), self);
        let row = |i: usize| m[i][0] * x + m[i][1] * y + m[i][2] * z + m[i][3];
        divide_by_w(Self::new(row(0), row(1), row(2)), row(3))
    }

    /// Sums each row in pairs, as pbrt-v4's `ApplyInverse` does.
    fn inverse_transformed(self, t: &Transform) -> Self {
        let (m, Point3 { x, y, z }) = (t.m_inv.rows(), self);
        let row = |i: usize| (m[i][0] * x + m[i][1] * y) + (m[i][2] * z + m[i][3]);
        let wp = row(3);
        debug_assert!(wp != 0.0, "apply_inverse: w = 0");
        divide_by_w(Self::new(row(0), row(1), row(2)), wp)
    }
}

/// `p / w`, or `p` itself when `w` is exactly 1.
fn divide_by_w<P: core::ops::Div<f32, Output = P>>(p: P, w: f32) -> P {
    #[expect(
        clippy::float_cmp,
        reason = "pbrt-v4 skips the divide only for an exact 1"
    )]
    let affine = w == 1.0;
    if affine { p } else { p / w }
}

/// `m` (the upper 3 × 3 part) times `(x, y, z)`.
fn mul_vector(m: &[[f32; 4]; 4], Vector3 { x, y, z }: Vector3f) -> Vector3f {
    let row = |i: usize| m[i][0] * x + m[i][1] * y + m[i][2] * z;
    Vector3f::new(row(0), row(1), row(2))
}

/// `mᵀ` (the upper 3 × 3 part) times `(x, y, z)`, the rule for normals.
fn mul_transposed(m: &[[f32; 4]; 4], Normal3 { x, y, z }: Normal3f) -> Normal3f {
    let col = |j: usize| m[0][j] * x + m[1][j] * y + m[2][j] * z;
    Normal3f::new(col(0), col(1), col(2))
}

impl Transformable for Vector3f {
    fn transformed(self, t: &Transform) -> Self {
        mul_vector(t.m.rows(), self)
    }

    fn inverse_transformed(self, t: &Transform) -> Self {
        mul_vector(t.m_inv.rows(), self)
    }
}

impl Transformable for Normal3f {
    fn transformed(self, t: &Transform) -> Self {
        mul_transposed(t.m_inv.rows(), self)
    }

    fn inverse_transformed(self, t: &Transform) -> Self {
        mul_transposed(t.m.rows(), self)
    }
}

/// Whether an error bound includes the translation column.
#[derive(Debug, Clone, Copy)]
enum Translation {
    /// Points move with the translation.
    Included,
    /// Vectors do not.
    Excluded,
}

/// The conservative error bound of `m` (upper 3 × 4 part) applied to the
/// point or vector with midpoint `c` and input error `e` (`None` when it is
/// exact): `γ₃` of the absolute products, plus `(1 + γ₃)` of the propagated
/// input error.
///
/// PBR 4e §6.8.5.
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

/// `m` applied to `p` with its error carried along.
///
/// pbrt-v4 `Transform::operator()(Point3fi)`. pbrt-v4's
/// `ApplyInverse(Point3fi)` leaves the translation out of the bound of an
/// exact point; Umbra uses this one rule both ways, so the inverse bound is
/// the conservative one.
fn transform_point3fi(m: &[[f32; 4]; 4], p: Point3fi) -> Point3fi {
    let Point3 { x, y, z } = Point3f::from(p);
    let row = |i: usize| (m[i][0] * x + m[i][1] * y) + (m[i][2] * z + m[i][3]);
    let e = (!p.is_exact()).then(|| Vector3f::from(p.error()));
    let error = transformed_error(m, Vector3f::new(x, y, z), e, Translation::Included);
    let pi = Point3fi::from_value_and_error(Point3f::new(row(0), row(1), row(2)), error.into());
    divide_by_w(pi, row(3))
}

impl Transformable for Point3fi {
    fn transformed(self, t: &Transform) -> Self {
        transform_point3fi(t.m.rows(), self)
    }

    fn inverse_transformed(self, t: &Transform) -> Self {
        transform_point3fi(t.m_inv.rows(), self)
    }
}

/// `m` applied to `v` with its error carried along.
///
/// pbrt-v4 `Transform::operator()(Vector3fi)`; pbrt-v4 has no inverse
/// overload, so the inverse is the same rule with the inverse matrix.
fn transform_vector3fi(m: &[[f32; 4]; 4], v: Vector3fi) -> Vector3fi {
    let c = Vector3f::from(v);
    let e = (!v.is_exact()).then(|| v.error());
    let error = transformed_error(m, c, e, Translation::Excluded);
    Vector3fi::from_value_and_error(mul_vector(m, c), error)
}

impl Transformable for Vector3fi {
    fn transformed(self, t: &Transform) -> Self {
        transform_vector3fi(t.m.rows(), self)
    }

    fn inverse_transformed(self, t: &Transform) -> Self {
        transform_vector3fi(t.m_inv.rows(), self)
    }
}

/// A translation by `delta`.
///
/// PBR 4e §3.9.2, pbrt-v4 `Translate`.
#[must_use]
pub fn translate(delta: Vector3f) -> Transform {
    let m = |d: Vector3f| {
        SquareMatrix::new([
            [1.0, 0.0, 0.0, d.x],
            [0.0, 1.0, 0.0, d.y],
            [0.0, 0.0, 1.0, d.z],
            [0.0, 0.0, 0.0, 1.0],
        ])
    };
    Transform::new(m(delta), m(-delta))
}

/// A scale by `x`, `y`, `z` along the axes.
///
/// PBR 4e §3.9.3, pbrt-v4 `Scale`.
#[must_use]
pub fn scale(x: f32, y: f32, z: f32) -> Transform {
    Transform::new(
        SquareMatrix::diag([x, y, z, 1.0]),
        SquareMatrix::diag([1.0 / x, 1.0 / y, 1.0 / z, 1.0]),
    )
}

/// A rotation with orthogonal matrix `m`, whose inverse is its transpose.
fn rotation(m: SquareMatrix<4>) -> Transform {
    Transform::new(m, m.transpose())
}

/// The sine and cosine of `theta` degrees.
fn sin_cos_degrees(theta: f32) -> (f32, f32) {
    (libm::sinf(radians(theta)), libm::cosf(radians(theta)))
}

/// A rotation by `theta` degrees about the x axis.
///
/// PBR 4e §3.9.5, pbrt-v4 `RotateX`.
#[must_use]
pub fn rotate_x(theta: f32) -> Transform {
    let (s, c) = sin_cos_degrees(theta);
    rotation(SquareMatrix::new([
        [1.0, 0.0, 0.0, 0.0],
        [0.0, c, -s, 0.0],
        [0.0, s, c, 0.0],
        [0.0, 0.0, 0.0, 1.0],
    ]))
}

/// A rotation by `theta` degrees about the y axis.
///
/// PBR 4e §3.9.5, pbrt-v4 `RotateY`.
#[must_use]
pub fn rotate_y(theta: f32) -> Transform {
    let (s, c) = sin_cos_degrees(theta);
    rotation(SquareMatrix::new([
        [c, 0.0, s, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [-s, 0.0, c, 0.0],
        [0.0, 0.0, 0.0, 1.0],
    ]))
}

/// A rotation by `theta` degrees about the z axis.
///
/// PBR 4e §3.9.5, pbrt-v4 `RotateZ`.
#[must_use]
pub fn rotate_z(theta: f32) -> Transform {
    let (s, c) = sin_cos_degrees(theta);
    rotation(SquareMatrix::new([
        [c, -s, 0.0, 0.0],
        [s, c, 0.0, 0.0],
        [0.0, 0.0, 1.0, 0.0],
        [0.0, 0.0, 0.0, 1.0],
    ]))
}

/// A rotation about `axis` by the angle with the given sine and cosine.
///
/// PBR 4e §3.9.6, pbrt-v4 `Rotate(Float sinTheta, Float cosTheta, Vector3f)`.
#[must_use]
pub fn rotate_sin_cos(sin_theta: f32, cos_theta: f32, axis: Vector3f) -> Transform {
    let a = axis.normalize();
    let (s, c) = (sin_theta, cos_theta);
    rotation(SquareMatrix::new([
        [
            a.x * a.x + (1.0 - a.x * a.x) * c,
            a.x * a.y * (1.0 - c) - a.z * s,
            a.x * a.z * (1.0 - c) + a.y * s,
            0.0,
        ],
        [
            a.x * a.y * (1.0 - c) + a.z * s,
            a.y * a.y + (1.0 - a.y * a.y) * c,
            a.y * a.z * (1.0 - c) - a.x * s,
            0.0,
        ],
        [
            a.x * a.z * (1.0 - c) - a.y * s,
            a.y * a.z * (1.0 - c) + a.x * s,
            a.z * a.z + (1.0 - a.z * a.z) * c,
            0.0,
        ],
        [0.0, 0.0, 0.0, 1.0],
    ]))
}

/// A rotation by `theta` degrees about `axis`.
///
/// PBR 4e §3.9.6, pbrt-v4 `Rotate(Float theta, Vector3f axis)`.
#[must_use]
pub fn rotate(theta: f32, axis: Vector3f) -> Transform {
    let (s, c) = sin_cos_degrees(theta);
    rotate_sin_cos(s, c, axis)
}

/// The rotation that takes the unit vector `from` to the unit vector `to`,
/// as two reflections.
///
/// PBR 4e §3.9.7, pbrt-v4 `RotateFromTo`.
#[must_use]
pub fn rotate_from_to(from: Vector3f, to: Vector3f) -> Transform {
    let refl = if from.x.abs() < 0.72 && to.x.abs() < 0.72 {
        Vector3f::new(1.0, 0.0, 0.0)
    } else if from.y.abs() < 0.72 && to.y.abs() < 0.72 {
        Vector3f::new(0.0, 1.0, 0.0)
    } else {
        Vector3f::new(0.0, 0.0, 1.0)
    };
    let (u, v) = (refl - from, refl - to);
    let (uu, vv, uv) = (dot(u, u), dot(v, v), dot(u, v));
    rotation(SquareMatrix::from_fn(|i, j| {
        let delta = if i == j { 1.0 } else { 0.0 };
        if i == 3 || j == 3 {
            return delta;
        }
        delta - 2.0 / uu * u[i] * u[j] - 2.0 / vv * v[i] * v[j] + 4.0 * uv / (uu * vv) * v[i] * u[j]
    }))
}

/// The camera-from-world transform of a camera at `pos` looking at `look`
/// with `up` as the up direction; `None` when `up` is parallel to the
/// viewing direction, where pbrt-v4 stops with a fatal error.
///
/// PBR 4e §3.9.8, pbrt-v4 `LookAt`.
#[must_use]
pub fn look_at(pos: Point3f, look: Point3f, up: Vector3f) -> Option<Transform> {
    let dir = (look - pos).normalize();
    if cross(up.normalize(), dir).length() == 0.0 {
        return None;
    }
    let right = cross(up.normalize(), dir).normalize();
    let new_up = cross(dir, right);
    let world_from_camera = SquareMatrix::new([
        [right.x, new_up.x, dir.x, pos.x],
        [right.y, new_up.y, dir.y, pos.y],
        [right.z, new_up.z, dir.z, pos.z],
        [0.0, 0.0, 0.0, 1.0],
    ]);
    let camera_from_world = world_from_camera.inverse()?;
    Some(Transform::new(camera_from_world, world_from_camera))
}

/// The orthographic projection that maps `z` in `[z_near, z_far]` to
/// `[0, 1]`.
///
/// PBR 4e §5.2.1, pbrt-v4 `Orthographic`.
#[must_use]
pub fn orthographic(z_near: f32, z_far: f32) -> Transform {
    scale(1.0, 1.0, 1.0 / (z_far - z_near)) * translate(Vector3f::new(0.0, 0.0, -z_near))
}

/// The perspective projection with a field of view of `fov` degrees, which
/// maps `z` in `[n, f]` to `[0, 1]` after the divide.
///
/// PBR 4e §5.2.2, pbrt-v4 `Perspective`.
#[must_use]
pub fn perspective(fov: f32, n: f32, f: f32) -> Transform {
    let persp = SquareMatrix::new([
        [1.0, 0.0, 0.0, 0.0],
        [0.0, 1.0, 0.0, 0.0],
        [0.0, 0.0, f / (f - n), -f * n / (f - n)],
        [0.0, 0.0, 1.0, 0.0],
    ]);
    let inv_tan_ang = 1.0 / libm::tanf(radians(fov) / 2.0);
    scale(inv_tan_ang, inv_tan_ang, 1.0) * Transform::from(persp)
}
