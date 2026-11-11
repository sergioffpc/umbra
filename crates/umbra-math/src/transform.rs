//! Affine and projective transformations (pbrt-v4 `Transform`,
//! `src/pbrt/util/transform.h`). Transforming a `Ray` or `Bounds3f` belongs
//! with those types, in `umbra-geometry`.
//!
//! Applying a transform runs as a [`Vec4`] kernel ([`kernels`]) with the
//! four rows of the matrix in the lanes; each computes the bits of its scalar
//! form in `tests/umbra_math/reference/transform.rs` (ADR 0007).

mod kernels;

use core::fmt;
use core::ops::Mul;

use crate::Vec4;
use crate::cmath;
use crate::math::radians;
use crate::square_matrix::SquareMatrix;
use crate::transcendental::sin_cos;
use crate::vecmath::{Frame, Normal3f, Point3f, Point3fi, Vector3f, Vector3fi, cross, dot};

/// A transformation and its inverse, as 4 × 4 matrices.
///
/// Each matrix is also kept by its columns, the layout the point kernels
/// read, so applying a transform never transposes: 256 bytes where the two
/// matrices alone are 128 (ADR 0007).
///
/// PBR 4e §3.9, pbrt-v4 `Transform`.
#[derive(Clone, Copy)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[cfg_attr(
    feature = "serde",
    serde(from = "TransformData", into = "TransformData")
)]
pub struct Transform {
    m: SquareMatrix<4>,
    m_inv: SquareMatrix<4>,
    /// The columns of `m`.
    m_columns: [Vec4; 4],
    /// The columns of `m_inv`.
    m_inv_columns: [Vec4; 4],
}

impl fmt::Debug for Transform {
    /// The two matrices; the columns are the same numbers again.
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Transform")
            .field("m", &self.m)
            .field("m_inv", &self.m_inv)
            .finish_non_exhaustive()
    }
}

impl Default for Transform {
    /// The identity.
    fn default() -> Self {
        Self::new(SquareMatrix::default(), SquareMatrix::default())
    }
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
        Self::new(m, m_inv)
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
        Self::new(self.m * t2.m, t2.m_inv * self.m_inv)
    }
}

impl Transform {
    /// The transform with matrix `m` and the given inverse.
    #[must_use]
    pub const fn new(m: SquareMatrix<4>, m_inv: SquareMatrix<4>) -> Self {
        Self {
            m_columns: kernels::columns(m.rows()),
            m_inv_columns: kernels::columns(m_inv.rows()),
            m,
            m_inv,
        }
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
            m_columns: self.m_inv_columns,
            m_inv_columns: self.m_columns,
        }
    }

    /// pbrt-v4 `Transpose(Transform)`.
    #[must_use]
    pub fn transpose(&self) -> Self {
        Self::new(self.m.transpose(), self.m_inv.transpose())
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
    #[inline]
    fn transformed(self, t: &Transform) -> Self {
        kernels::point_left_to_right(&t.m_columns, self)
    }

    /// Sums each row in pairs, as pbrt-v4's `ApplyInverse` does.
    #[inline]
    fn inverse_transformed(self, t: &Transform) -> Self {
        kernels::point_pairwise(&t.m_inv_columns, self)
    }
}

impl Transformable for Vector3f {
    /// The upper 3 × 3 part, each row summed left to right.
    #[inline]
    fn transformed(self, t: &Transform) -> Self {
        kernels::vector(&t.m_columns, self)
    }

    #[inline]
    fn inverse_transformed(self, t: &Transform) -> Self {
        kernels::vector(&t.m_inv_columns, self)
    }
}

impl Transformable for Normal3f {
    /// The inverse transpose.
    #[inline]
    fn transformed(self, t: &Transform) -> Self {
        kernels::normal_transposed(t.m_inv.rows(), self)
    }

    #[inline]
    fn inverse_transformed(self, t: &Transform) -> Self {
        kernels::normal_transposed(t.m.rows(), self)
    }
}

impl Transformable for Point3fi {
    /// pbrt-v4 `Transform::operator()(Point3fi)`: the point with its error
    /// carried along (PBR 4e §6.8.5).
    #[inline]
    fn transformed(self, t: &Transform) -> Self {
        kernels::point3fi(&t.m_columns, self)
    }

    /// The same rule with the inverse matrix. pbrt-v4's
    /// `ApplyInverse(Point3fi)` leaves the translation out of the bound of an
    /// exact point; Umbra uses one rule both ways, so the inverse bound is
    /// the conservative one.
    #[inline]
    fn inverse_transformed(self, t: &Transform) -> Self {
        kernels::point3fi(&t.m_inv_columns, self)
    }
}

impl Transformable for Vector3fi {
    /// pbrt-v4 `Transform::operator()(Vector3fi)`.
    #[inline]
    fn transformed(self, t: &Transform) -> Self {
        kernels::vector3fi(&t.m_columns, self)
    }

    /// pbrt-v4 has no inverse overload; the same rule with the inverse
    /// matrix.
    #[inline]
    fn inverse_transformed(self, t: &Transform) -> Self {
        kernels::vector3fi(&t.m_inv_columns, self)
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
    sin_cos(radians(theta))
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
    let up_cross_dir = cross(up.normalize(), dir);
    if up_cross_dir.length() == 0.0 {
        return None;
    }
    let right = up_cross_dir.normalize();
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
    let inv_tan_ang = 1.0 / cmath::tanf(radians(fov) / 2.0);
    scale(inv_tan_ang, inv_tan_ang, 1.0) * Transform::from(persp)
}
