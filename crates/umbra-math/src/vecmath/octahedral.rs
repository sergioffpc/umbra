//! Unit vectors in 32 bits (pbrt-v4 `OctahedralVector`).

use crate::math::clamp;
use crate::vecmath::vector::Vector3f;

/// A unit vector packed into two 16-bit coordinates on the octahedron
/// `|x| + |y| + |z| = 1`, unfolded onto the plane.
///
/// PBR 4e §3.8.4, pbrt-v4 `OctahedralVector`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Hash)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct OctahedralVector {
    x: u16,
    y: u16,
}

/// `±1` with the sign of `v`.
fn sign(v: f32) -> f32 {
    1.0_f32.copysign(v)
}

/// `f` in `[−1, 1]` quantised to 16 bits.
#[expect(clippy::manual_midpoint, reason = "pbrt-v4's `(f + 1) / 2`")]
fn encode(f: f32) -> u16 {
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_sign_loss,
        reason = "the value is rounded and in [0, 65535]"
    )]
    let quantised = libm::roundf(clamp((f + 1.0) / 2.0, 0.0, 1.0) * 65535.0) as u16;
    quantised
}

impl From<Vector3f> for OctahedralVector {
    fn from(v: Vector3f) -> Self {
        let v = v / (v.x.abs() + v.y.abs() + v.z.abs());
        if v.z >= 0.0 {
            Self {
                x: encode(v.x),
                y: encode(v.y),
            }
        } else {
            // Fold the lower hemisphere over the diagonals.
            Self {
                x: encode((1.0 - v.y.abs()) * sign(v.x)),
                y: encode((1.0 - v.x.abs()) * sign(v.y)),
            }
        }
    }
}

impl From<OctahedralVector> for Vector3f {
    /// The decoded unit vector.
    fn from(o: OctahedralVector) -> Self {
        let x = -1.0 + 2.0 * (f32::from(o.x) / 65535.0);
        let y = -1.0 + 2.0 * (f32::from(o.y) / 65535.0);
        let z = 1.0 - (x.abs() + y.abs());
        let v = if z < 0.0 {
            // Unfold the lower hemisphere.
            Self::new((1.0 - y.abs()) * sign(x), (1.0 - x.abs()) * sign(y), z)
        } else {
            Self::new(x, y, z)
        };
        v.normalize()
    }
}
