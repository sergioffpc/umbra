//! Vectors (pbrt-v4 `Vector2`, `Vector3`, `Vector3fi`).

use crate::math::{Real, safe_asin, sum_of_products};
use crate::vecmath::tuple::{impl_add_sub, impl_tuple2, impl_tuple3};
use crate::{Interval, PI};

/// A 2D vector.
///
/// PBR 4e §3.3, pbrt-v4 `Vector2`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Hash)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[repr(C)]
pub struct Vector2<T> {
    /// The x component.
    pub x: T,
    /// The y component.
    pub y: T,
}

/// A 3D vector.
///
/// PBR 4e §3.3, pbrt-v4 `Vector3`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Hash)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[repr(C)]
pub struct Vector3<T> {
    /// The x component.
    pub x: T,
    /// The y component.
    pub y: T,
    /// The z component.
    pub z: T,
}

/// pbrt-v4 `Vector2f`.
pub type Vector2f = Vector2<f32>;
/// pbrt-v4 `Vector2i`.
pub type Vector2i = Vector2<i32>;
/// pbrt-v4 `Vector3f`.
pub type Vector3f = Vector3<f32>;
/// pbrt-v4 `Vector3i`.
pub type Vector3i = Vector3<i32>;
/// A vector of intervals that bound its components' error.
///
/// PBR 4e §6.8.4, pbrt-v4 `Vector3fi`.
pub type Vector3fi = Vector3<Interval>;

// The host copies arena slices of these into GPU buffers byte for byte
// (ADR 0028, arc42 §8.12): their layout is the same on `x86_64` and `nvptx64`.
const _: () = {
    use core::mem::offset_of;

    assert!(
        size_of::<Vector2f>() == 8,
        "`Vector2f` is 8 bytes, with no padding"
    );
    assert!(offset_of!(Vector2f, x) == 0, "`Vector2f::x` is at byte 0");
    assert!(offset_of!(Vector2f, y) == 4, "`Vector2f::y` is at byte 4");
    assert!(
        size_of::<Vector3f>() == 12,
        "`Vector3f` is 12 bytes, with no padding"
    );
    assert!(offset_of!(Vector3f, x) == 0, "`Vector3f::x` is at byte 0");
    assert!(offset_of!(Vector3f, y) == 4, "`Vector3f::y` is at byte 4");
    assert!(offset_of!(Vector3f, z) == 8, "`Vector3f::z` is at byte 8");
};

impl_tuple2!(Vector2);
impl_tuple3!(Vector3);
impl_add_sub!(Vector2 + Vector2 = Vector2, x, y);
impl_add_sub!(Vector2 - Vector2 = Vector2, x, y);
impl_add_sub!(Vector2 += Vector2, x, y);
impl_add_sub!(Vector2 -= Vector2, x, y);
impl_add_sub!(Vector3 + Vector3 = Vector3, x, y, z);
impl_add_sub!(Vector3 - Vector3 = Vector3, x, y, z);
impl_add_sub!(Vector3 += Vector3, x, y, z);
impl_add_sub!(Vector3 -= Vector3, x, y, z);

impl<T: Real> Vector2<T> {
    /// pbrt-v4 `LengthSquared(Vector2)`.
    #[must_use]
    pub fn length_squared(self) -> T {
        self.x.sqr() + self.y.sqr()
    }

    /// pbrt-v4 `Length(Vector2)`.
    #[must_use]
    pub fn length(self) -> T {
        self.length_squared().sqrt()
    }

    /// The vector divided by its length.
    ///
    /// pbrt-v4 `Normalize(Vector2)`.
    #[must_use]
    pub fn normalize(self) -> Self {
        self / self.length()
    }
}

impl<T: Real> Vector3<T> {
    /// pbrt-v4 `LengthSquared(Vector3)`.
    #[must_use]
    pub fn length_squared(self) -> T {
        self.x.sqr() + self.y.sqr() + self.z.sqr()
    }

    /// pbrt-v4 `Length(Vector3)`.
    #[must_use]
    pub fn length(self) -> T {
        self.length_squared().sqrt()
    }

    /// The vector divided by its length.
    ///
    /// pbrt-v4 `Normalize(Vector3)`.
    #[must_use]
    pub fn normalize(self) -> Self {
        self / self.length()
    }
}

/// The dot product, for each pair of vector and normal types pbrt-v4
/// defines it on.
///
/// PBR 4e §3.3.2.
pub trait Dot<Rhs = Self> {
    /// The scalar type of the product.
    type Output;
    /// pbrt-v4 `Dot`.
    #[must_use]
    fn dot(self, rhs: Rhs) -> Self::Output;
}

/// The dot product of `a` and `b`.
///
/// PBR 4e §3.3.2, pbrt-v4 `Dot`.
#[inline]
#[must_use]
pub fn dot<A: Dot<B>, B>(a: A, b: B) -> A::Output {
    a.dot(b)
}

/// `|a · b|`.
///
/// PBR 4e §3.3.2, pbrt-v4 `AbsDot`.
#[inline]
#[must_use]
pub fn abs_dot<A: Dot<B, Output = T>, B, T: Real>(a: A, b: B) -> T {
    a.dot(b).abs()
}

impl<T: Real> Dot for Vector2<T> {
    type Output = T;
    /// With `SumOfProducts`, as pbrt-v4's `Vector2` overload.
    #[inline]
    fn dot(self, w: Self) -> T {
        sum_of_products(self.x, w.x, self.y, w.y)
    }
}

impl<T: Real> Dot for Vector3<T> {
    type Output = T;
    /// The plain `x·x' + y·y' + z·z'` of pbrt-v4's `Vector3` overload.
    #[inline]
    fn dot(self, w: Self) -> T {
        self.x * w.x + self.y * w.y + self.z * w.z
    }
}

/// The cross product, for each pair of vector and normal types pbrt-v4
/// defines it on.
///
/// PBR 4e §3.3.2.
pub trait Cross<Rhs = Self> {
    /// pbrt-v4 `Cross`.
    #[must_use]
    fn cross(self, rhs: Rhs) -> Vector3<Self::Scalar>;
    /// The element type.
    type Scalar;
}

/// The cross product `a × b`, each component by `DifferenceOfProducts`.
///
/// PBR 4e §3.3.2, pbrt-v4 `Cross`.
#[inline]
#[must_use]
pub fn cross<A: Cross<B>, B>(a: A, b: B) -> Vector3<A::Scalar> {
    a.cross(b)
}

/// `a × b` from the components of two 3-tuples.
pub(crate) fn cross_components<T: Real>(a: [T; 3], b: [T; 3]) -> Vector3<T> {
    let [ax, ay, az] = a;
    let [bx, by, bz] = b;
    Vector3 {
        x: T::difference_of_products(ay, bz, az, by),
        y: T::difference_of_products(az, bx, ax, bz),
        z: T::difference_of_products(ax, by, ay, bx),
    }
}

impl<T: Real> Cross for Vector3<T> {
    type Scalar = T;
    #[inline]
    fn cross(self, w: Self) -> Self {
        cross_components([self.x, self.y, self.z], [w.x, w.y, w.z])
    }
}

/// `v` flipped, if needed, to lie in the hemisphere of `w`.
///
/// PBR 4e §3.5.3.
pub trait FaceForward<Rhs> {
    /// pbrt-v4 `FaceForward`.
    #[must_use]
    fn face_forward(self, w: Rhs) -> Self;
}

/// `a`, negated if it points away from `b`.
///
/// PBR 4e §3.5.3, pbrt-v4 `FaceForward`.
#[inline]
#[must_use]
pub fn face_forward<A: FaceForward<B>, B>(a: A, b: B) -> A {
    a.face_forward(b)
}

impl FaceForward<Self> for Vector3f {
    #[inline]
    fn face_forward(self, w: Self) -> Self {
        if dot(self, w) < 0.0 { -self } else { self }
    }
}

/// The angle between two unit vectors or two unit normals, accurate also
/// when they are nearly parallel or opposite.
///
/// PBR 4e §3.3.2.
pub trait AngleBetween {
    /// pbrt-v4 `AngleBetween`.
    #[must_use]
    fn angle_between(self, other: Self) -> f32;
}

/// The angle in radians between the unit vectors (or unit normals) `a` and
/// `b`: `acos(a · b)`, computed stably from `|a ± b|`.
///
/// PBR 4e §3.3.2, pbrt-v4 `AngleBetween`.
#[inline]
#[must_use]
pub fn angle_between<V: AngleBetween>(a: V, b: V) -> f32 {
    a.angle_between(b)
}

impl AngleBetween for Vector3f {
    fn angle_between(self, w: Self) -> f32 {
        if dot(self, w) < 0.0 {
            PI - 2.0 * safe_asin((self + w).length() / 2.0)
        } else {
            2.0 * safe_asin((w - self).length() / 2.0)
        }
    }
}

/// `v` minus its projection on the unit vector `w`.
///
/// PBR 4e §3.3.3, pbrt-v4 `GramSchmidt`.
#[must_use]
pub fn gram_schmidt(v: Vector3f, w: Vector3f) -> Vector3f {
    v - dot(v, w) * w
}

/// Two unit vectors that complete the unit vector `v` to an orthonormal
/// basis (Duff et al. 2017).
///
/// PBR 4e §3.3.3, pbrt-v4 `CoordinateSystem`.
#[must_use]
pub fn coordinate_system(v: impl Into<Vector3f>) -> (Vector3f, Vector3f) {
    let v1: Vector3f = v.into();
    let sign = 1.0_f32.copysign(v1.z);
    let a = -1.0 / (sign + v1.z);
    let b = v1.x * v1.y * a;
    (
        Vector3f::new(1.0 + sign * v1.x.sqr() * a, sign * b, -sign * v1.x),
        Vector3f::new(b, sign + v1.y.sqr() * a, -v1.y),
    )
}
