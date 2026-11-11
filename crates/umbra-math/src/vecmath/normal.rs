//! Surface normals (pbrt-v4 `Normal3`).

use crate::math::{Real, sum_of_products};
use crate::vecmath::tuple::{impl_add_sub, impl_convert, impl_tuple3};
use crate::vecmath::vector::{
    AngleBetween, Cross, Dot, FaceForward, Vector3, Vector3f, cross_components,
};
use crate::{PI, dot, safe_asin};

/// A surface normal: like a vector, but transformed by the inverse transpose
/// and not necessarily of unit length.
///
/// PBR 4e §3.5, pbrt-v4 `Normal3`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Hash)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[repr(C)]
pub struct Normal3<T> {
    /// The x component.
    pub x: T,
    /// The y component.
    pub y: T,
    /// The z component.
    pub z: T,
}

/// pbrt-v4 `Normal3f`.
pub type Normal3f = Normal3<f32>;

// The host copies arena slices of these into GPU buffers byte for byte
// (ADR 0028, arc42 §8.12): their layout is the same on `x86_64` and `nvptx64`.
const _: () = {
    use core::mem::offset_of;

    assert!(
        size_of::<Normal3f>() == 12,
        "`Normal3f` is 12 bytes, with no padding"
    );
    assert!(offset_of!(Normal3f, x) == 0, "`Normal3f::x` is at byte 0");
    assert!(offset_of!(Normal3f, y) == 4, "`Normal3f::y` is at byte 4");
    assert!(offset_of!(Normal3f, z) == 8, "`Normal3f::z` is at byte 8");
};

impl_tuple3!(Normal3);
impl_add_sub!(Normal3 + Normal3 = Normal3, x, y, z);
impl_add_sub!(Normal3 - Normal3 = Normal3, x, y, z);
impl_add_sub!(Normal3 += Normal3, x, y, z);
impl_add_sub!(Normal3 -= Normal3, x, y, z);
impl_convert!(Normal3 => Vector3, x, y, z);
impl_convert!(Vector3 => Normal3, x, y, z);

impl<T: Real> Normal3<T> {
    /// pbrt-v4 `LengthSquared(Normal3)`.
    #[must_use]
    pub fn length_squared(self) -> T {
        self.x.sqr() + self.y.sqr() + self.z.sqr()
    }

    /// pbrt-v4 `Length(Normal3)`.
    #[must_use]
    pub fn length(self) -> T {
        self.length_squared().sqrt()
    }

    /// The normal divided by its length.
    ///
    /// pbrt-v4 `Normalize(Normal3)`.
    #[must_use]
    pub fn normalize(self) -> Self {
        self / self.length()
    }
}

/// `n.x·v.x + (n.y·v.y + n.z·v.z)` with FMA, as every pbrt-v4 dot product
/// that involves a normal is computed.
fn fma_dot<T: Real>(n: [T; 3], v: [T; 3]) -> T {
    T::fma(n[0], v[0], sum_of_products(n[1], v[1], n[2], v[2]))
}

impl<T: Real> Dot<Vector3<T>> for Normal3<T> {
    type Output = T;
    #[inline]
    fn dot(self, v: Vector3<T>) -> T {
        fma_dot([self.x, self.y, self.z], [v.x, v.y, v.z])
    }
}

impl<T: Real> Dot<Normal3<T>> for Vector3<T> {
    type Output = T;
    #[inline]
    fn dot(self, n: Normal3<T>) -> T {
        fma_dot([n.x, n.y, n.z], [self.x, self.y, self.z])
    }
}

impl<T: Real> Dot for Normal3<T> {
    type Output = T;
    #[inline]
    fn dot(self, n: Self) -> T {
        fma_dot([self.x, self.y, self.z], [n.x, n.y, n.z])
    }
}

impl<T: Real> Cross<Normal3<T>> for Vector3<T> {
    type Scalar = T;
    #[inline]
    fn cross(self, n: Normal3<T>) -> Vector3<T> {
        cross_components([self.x, self.y, self.z], [n.x, n.y, n.z])
    }
}

impl<T: Real> Cross<Vector3<T>> for Normal3<T> {
    type Scalar = T;
    #[inline]
    fn cross(self, v: Vector3<T>) -> Vector3<T> {
        cross_components([self.x, self.y, self.z], [v.x, v.y, v.z])
    }
}

impl FaceForward<Vector3f> for Normal3f {
    #[inline]
    fn face_forward(self, v: Vector3f) -> Self {
        if dot(self, v) < 0.0 { -self } else { self }
    }
}

impl FaceForward<Self> for Normal3f {
    #[inline]
    fn face_forward(self, n: Self) -> Self {
        if dot(self, n) < 0.0 { -self } else { self }
    }
}

impl FaceForward<Normal3f> for Vector3f {
    #[inline]
    fn face_forward(self, n: Normal3f) -> Self {
        if dot(self, n) < 0.0 { -self } else { self }
    }
}

impl AngleBetween for Normal3f {
    fn angle_between(self, b: Self) -> f32 {
        if dot(self, b) < 0.0 {
            PI - 2.0 * safe_asin((self + b).length() / 2.0)
        } else {
            2.0 * safe_asin((b - self).length() / 2.0)
        }
    }
}
