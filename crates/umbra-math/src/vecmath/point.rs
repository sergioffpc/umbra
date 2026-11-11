//! Points (pbrt-v4 `Point2`, `Point3`, `Point3fi`).

use crate::Interval;
use crate::math::Real;
use crate::vecmath::tuple::{impl_add_sub, impl_convert, impl_tuple2, impl_tuple3};
use crate::vecmath::vector::{Vector2, Vector3};

/// A 2D point.
///
/// PBR 4e §3.4, pbrt-v4 `Point2`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Hash)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[repr(C)]
pub struct Point2<T> {
    /// The x coordinate.
    pub x: T,
    /// The y coordinate.
    pub y: T,
}

/// A 3D point.
///
/// PBR 4e §3.4, pbrt-v4 `Point3`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Hash)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[repr(C)]
pub struct Point3<T> {
    /// The x coordinate.
    pub x: T,
    /// The y coordinate.
    pub y: T,
    /// The z coordinate.
    pub z: T,
}

/// pbrt-v4 `Point2f`.
pub type Point2f = Point2<f32>;
/// pbrt-v4 `Point2i`.
pub type Point2i = Point2<i32>;
/// pbrt-v4 `Point3f`.
pub type Point3f = Point3<f32>;
/// pbrt-v4 `Point3i`.
pub type Point3i = Point3<i32>;
/// A point whose coordinates are intervals that bound its error, as shape
/// intersections return it.
///
/// PBR 4e §6.8.4, pbrt-v4 `Point3fi`.
pub type Point3fi = Point3<Interval>;

impl_tuple2!(Point2);
// The host copies arena slices of these into GPU buffers byte for byte
// (ADR 0028, arc42 §8.12): their layout is the same on `x86_64` and `nvptx64`.
const _: () = {
    use core::mem::offset_of;

    assert!(
        size_of::<Point2f>() == 8,
        "`Point2f` is 8 bytes, with no padding"
    );
    assert!(offset_of!(Point2f, x) == 0, "`Point2f::x` is at byte 0");
    assert!(offset_of!(Point2f, y) == 4, "`Point2f::y` is at byte 4");
    assert!(
        size_of::<Point3f>() == 12,
        "`Point3f` is 12 bytes, with no padding"
    );
    assert!(offset_of!(Point3f, x) == 0, "`Point3f::x` is at byte 0");
    assert!(offset_of!(Point3f, y) == 4, "`Point3f::y` is at byte 4");
    assert!(offset_of!(Point3f, z) == 8, "`Point3f::z` is at byte 8");
};

impl_tuple3!(Point3);
// A point plus a vector, or a weighted sum of points, is a point; the
// difference of two points is a vector.
impl_add_sub!(Point2 + Vector2 = Point2, x, y);
impl_add_sub!(Point2 + Point2 = Point2, x, y);
impl_add_sub!(Point2 - Vector2 = Point2, x, y);
impl_add_sub!(Point2 - Point2 = Vector2, x, y);
impl_add_sub!(Point2 += Vector2, x, y);
impl_add_sub!(Point2 -= Vector2, x, y);
impl_add_sub!(Point3 + Vector3 = Point3, x, y, z);
impl_add_sub!(Point3 + Point3 = Point3, x, y, z);
impl_add_sub!(Point3 - Vector3 = Point3, x, y, z);
impl_add_sub!(Point3 - Point3 = Vector3, x, y, z);
impl_add_sub!(Point3 += Vector3, x, y, z);
impl_add_sub!(Point3 -= Vector3, x, y, z);
impl_convert!(Point2 => Vector2, x, y);
impl_convert!(Vector2 => Point2, x, y);
impl_convert!(Point3 => Vector3, x, y, z);
impl_convert!(Vector3 => Point3, x, y, z);

/// The distance between two points.
///
/// PBR 4e §3.4.
pub trait Distance: Copy {
    /// The element type.
    type Scalar;
    /// pbrt-v4 `DistanceSquared`.
    #[must_use]
    fn distance_squared(self, other: Self) -> Self::Scalar;
    /// pbrt-v4 `Distance`.
    #[must_use]
    fn distance(self, other: Self) -> Self::Scalar;
}

impl<T: Real> Distance for Point2<T> {
    type Scalar = T;
    fn distance_squared(self, p: Self) -> T {
        (self - p).length_squared()
    }

    fn distance(self, p: Self) -> T {
        (self - p).length()
    }
}

impl<T: Real> Distance for Point3<T> {
    type Scalar = T;
    fn distance_squared(self, p: Self) -> T {
        (self - p).length_squared()
    }

    fn distance(self, p: Self) -> T {
        (self - p).length()
    }
}

/// The distance between `a` and `b`.
///
/// PBR 4e §3.4, pbrt-v4 `Distance`.
#[inline]
#[must_use]
pub fn distance<P: Distance>(a: P, b: P) -> P::Scalar {
    a.distance(b)
}

/// The squared distance between `a` and `b`.
///
/// PBR 4e §3.4, pbrt-v4 `DistanceSquared`.
#[inline]
#[must_use]
pub fn distance_squared<P: Distance>(a: P, b: P) -> P::Scalar {
    a.distance_squared(b)
}
