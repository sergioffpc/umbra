//! Orthonormal coordinate frames (pbrt-v4 `Frame`).

use crate::vecmath::normal::Normal3f;
use crate::vecmath::vector::{Vector3f, coordinate_system, cross, dot};

/// An orthonormal basis, for moving directions in and out of a local
/// coordinate system such as the shading frame.
///
/// PBR 4e §3.3.3, pbrt-v4 `Frame`.
#[derive(Debug, Clone, Copy, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[repr(C)]
pub struct Frame {
    /// The first axis.
    pub x: Vector3f,
    /// The second axis.
    pub y: Vector3f,
    /// The third axis.
    pub z: Vector3f,
}

// The host copies arena slices of these into GPU buffers byte for byte
// (ADR 0028, arc42 §8.12): their layout is the same on `x86_64` and `nvptx64`.
const _: () = {
    use core::mem::offset_of;

    assert!(
        size_of::<Frame>() == 36,
        "`Frame` is 36 bytes, with no padding"
    );
    assert!(offset_of!(Frame, x) == 0, "`Frame::x` is at byte 0");
    assert!(offset_of!(Frame, y) == 12, "`Frame::y` is at byte 12");
    assert!(offset_of!(Frame, z) == 24, "`Frame::z` is at byte 24");
};

impl Default for Frame {
    /// The standard basis.
    fn default() -> Self {
        Self {
            x: Vector3f::new(1.0, 0.0, 0.0),
            y: Vector3f::new(0.0, 1.0, 0.0),
            z: Vector3f::new(0.0, 0.0, 1.0),
        }
    }
}

impl Frame {
    /// The frame with the given axes, which must be orthonormal.
    #[must_use]
    pub fn new(x: Vector3f, y: Vector3f, z: Vector3f) -> Self {
        debug_assert!(
            is_orthonormal(x, y, z),
            "not orthonormal: {x:?} {y:?} {z:?}"
        );
        Self { x, y, z }
    }

    /// pbrt-v4 `Frame::FromXZ`.
    #[must_use]
    pub fn from_xz(x: Vector3f, z: Vector3f) -> Self {
        Self::new(x, cross(z, x), z)
    }

    /// pbrt-v4 `Frame::FromXY`.
    #[must_use]
    pub fn from_xy(x: Vector3f, y: Vector3f) -> Self {
        Self::new(x, y, cross(x, y))
    }

    /// A frame whose `z` axis is the unit vector or normal `z`.
    ///
    /// pbrt-v4 `Frame::FromZ`.
    #[must_use]
    pub fn from_z(z: impl Into<Vector3f>) -> Self {
        let z = z.into();
        let (x, y) = coordinate_system(z);
        Self::new(x, y, z)
    }

    /// A frame whose `x` axis is the unit vector or normal `x`.
    ///
    /// pbrt-v4 `Frame::FromX`.
    #[must_use]
    pub fn from_x(x: impl Into<Vector3f>) -> Self {
        let x = x.into();
        let (y, z) = coordinate_system(x);
        Self::new(x, y, z)
    }

    /// A frame whose `y` axis is the unit vector or normal `y`.
    ///
    /// pbrt-v4 `Frame::FromY`.
    #[must_use]
    pub fn from_y(y: impl Into<Vector3f>) -> Self {
        let y = y.into();
        let (z, x) = coordinate_system(y);
        Self::new(x, y, z)
    }

    /// `v` in this frame's coordinates.
    ///
    /// pbrt-v4 `Frame::ToLocal`.
    #[must_use]
    pub fn to_local<V: InFrame>(self, v: V) -> V {
        v.in_frame(self)
    }

    /// `v`, given in this frame's coordinates, back in the outer ones.
    ///
    /// pbrt-v4 `Frame::FromLocal`.
    #[must_use]
    #[expect(clippy::wrong_self_convention, reason = "pbrt-v4's `Frame::FromLocal`")]
    pub fn from_local<V: InFrame>(self, v: V) -> V {
        v.out_of_frame(self)
    }
}

/// Whether the axes are unit length and mutually perpendicular, to the
/// tolerance of pbrt-v4's checks.
fn is_orthonormal(x: Vector3f, y: Vector3f, z: Vector3f) -> bool {
    let unit = |v: Vector3f| (v.length_squared() - 1.0).abs() < 1e-4;
    let perpendicular = |a: Vector3f, b: Vector3f| dot(a, b).abs() < 1e-4;
    unit(x)
        && unit(y)
        && unit(z)
        && perpendicular(x, y)
        && perpendicular(y, z)
        && perpendicular(z, x)
}

/// The directions a [`Frame`] maps: vectors and normals, each by pbrt-v4's
/// own overload.
pub trait InFrame: Copy {
    /// `self` in the frame's coordinates (pbrt-v4 `Frame::ToLocal`).
    #[must_use]
    fn in_frame(self, frame: Frame) -> Self;
    /// `self`, given in the frame's coordinates, in the outer ones (pbrt-v4
    /// `Frame::FromLocal`).
    #[must_use]
    fn out_of_frame(self, frame: Frame) -> Self;
}

impl InFrame for Vector3f {
    fn in_frame(self, f: Frame) -> Self {
        Self::new(dot(self, f.x), dot(self, f.y), dot(self, f.z))
    }

    fn out_of_frame(self, f: Frame) -> Self {
        self.x * f.x + self.y * f.y + self.z * f.z
    }
}

impl InFrame for Normal3f {
    fn in_frame(self, f: Frame) -> Self {
        Self::new(dot(self, f.x), dot(self, f.y), dot(self, f.z))
    }

    fn out_of_frame(self, f: Frame) -> Self {
        Self::from(self.x * f.x + self.y * f.y + self.z * f.z)
    }
}
