//! Axis-aligned bounding boxes (pbrt-v4 `Bounds2`, `Bounds3`,
//! `src/pbrt/util/vecmath.h`).

use umbra_math::{Point2f, Point3f, Transform, lerp};

/// The axis-aligned box from `p_min` to `p_max`, both included.
///
/// An arena type, stored with what it bounds: `#[repr(C)]`, with the layout
/// assertions below (ADR 0028).
///
/// PBR 4e §3.7, pbrt-v4 `Bounds3f`.
#[derive(Debug, Clone, Copy, PartialEq)]
#[repr(C)]
pub struct Bounds3f {
    /// The corner with the smallest coordinates.
    pub p_min: Point3f,
    /// The corner with the largest coordinates.
    pub p_max: Point3f,
}

// The host copies arena slices of these into GPU buffers byte for byte
// (ADR 0028, arc42 §8.12): their layout is the same on `x86_64` and `nvptx64`.
const _: () = {
    use core::mem::offset_of;

    assert!(size_of::<Bounds3f>() == 24, "`Bounds3f` is 24 bytes");
    assert!(align_of::<Bounds3f>() == 4, "`Bounds3f` is 4-byte aligned");
    assert!(
        offset_of!(Bounds3f, p_min) == 0,
        "`Bounds3f::p_min` is at byte 0"
    );
    assert!(
        offset_of!(Bounds3f, p_max) == 12,
        "`Bounds3f::p_max` is at byte 12"
    );
};

impl Bounds3f {
    /// The box with `p1` and `p2` at opposite corners, in any order.
    ///
    /// pbrt-v4 `Bounds3(Point3, Point3)`.
    #[inline]
    #[must_use]
    pub fn new(p1: Point3f, p2: Point3f) -> Self {
        Self {
            p_min: p1.min(p2),
            p_max: p1.max(p2),
        }
    }

    /// The box around the eight corners of this one transformed by `t`.
    ///
    /// pbrt-v4 `Transform::operator()(const Bounds3f &)`.
    #[must_use]
    pub fn transform(&self, t: &Transform) -> Self {
        let first = t.apply(self.corner(0));
        (1..8).fold(Self::new(first, first), |bounds, i| {
            let p = t.apply(self.corner(i));
            Self {
                p_min: bounds.p_min.min(p),
                p_max: bounds.p_max.max(p),
            }
        })
    }

    /// Corner `i` of the box, for `i` in `0..8`: bit 0 picks `x` from
    /// `p_max`, bit 1 `y` and bit 2 `z`.
    ///
    /// pbrt-v4 `Bounds3::Corner`.
    fn corner(&self, i: usize) -> Point3f {
        let pick = |bit: usize, low: f32, high: f32| if i & bit == 0 { low } else { high };
        Point3f::new(
            pick(1, self.p_min.x, self.p_max.x),
            pick(2, self.p_min.y, self.p_max.y),
            pick(4, self.p_min.z, self.p_max.z),
        )
    }
}

/// The axis-aligned rectangle from `p_min` to `p_max`, both included.
///
/// PBR 4e §3.7, pbrt-v4 `Bounds2f`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Bounds2f {
    /// The corner with the smallest coordinates.
    pub p_min: Point2f,
    /// The corner with the largest coordinates.
    pub p_max: Point2f,
}

impl Bounds2f {
    /// The rectangle with `p1` and `p2` at opposite corners, in any order.
    ///
    /// pbrt-v4 `Bounds2(Point2, Point2)`.
    #[inline]
    #[must_use]
    pub fn new(p1: Point2f, p2: Point2f) -> Self {
        Self {
            p_min: p1.min(p2),
            p_max: p1.max(p2),
        }
    }

    /// The point at the fractions `t` of the way from `p_min` to `p_max`,
    /// per axis.
    ///
    /// pbrt-v4 `Bounds2::Lerp`.
    #[inline]
    #[must_use]
    pub fn lerp(&self, t: Point2f) -> Point2f {
        Point2f::new(
            lerp(t.x, self.p_min.x, self.p_max.x),
            lerp(t.y, self.p_min.y, self.p_max.y),
        )
    }
}
