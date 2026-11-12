//! Rays (pbrt-v4 `Ray`, `src/pbrt/ray.h`).

use umbra_math::{Point3f, Point3fi, Transform, Vector3f, Vector3fi, dot};

/// A semi-infinite line: the points `o + t·d` for `t ≥ 0`.
///
/// A per-sample value, so it keeps `repr(Rust)` (ADR 0028). pbrt-v4's `time`
/// and `medium` come with the features that read them.
///
/// PBR 4e §3.1, pbrt-v4 `Ray`.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Ray {
    /// The origin.
    pub o: Point3f,
    /// The direction, not necessarily normalised.
    pub d: Vector3f,
}

impl Ray {
    /// The ray from `o` along `d`.
    #[inline]
    #[must_use]
    pub const fn new(o: Point3f, d: Vector3f) -> Self {
        Self { o, d }
    }

    /// The point at parameter `t` along the ray.
    ///
    /// pbrt-v4 `Ray::operator()`.
    #[inline]
    #[must_use]
    pub fn at(&self, t: f32) -> Point3f {
        self.o + self.d * t
    }

    /// The ray transformed by `t`, its origin moved along its direction to
    /// the far edge of the origin's rounding error, so the transformed ray
    /// cannot start behind the surface it left.
    ///
    /// PBR 4e §6.8.6, pbrt-v4 `Transform::operator()(const Ray &)`.
    #[must_use]
    pub fn transform(&self, t: &Transform) -> Self {
        let mut o = t.apply(Point3fi::from(self.o));
        let d = t.apply(self.d);
        let length_squared = d.length_squared();
        if length_squared > 0.0 {
            let dt = dot(d.abs(), Vector3f::from(o.error())) / length_squared;
            o += Vector3fi::from(d * dt);
        }
        Self::new(Point3f::from(o), d)
    }
}
