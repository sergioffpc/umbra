//! Rays (pbrt-v4 `Ray`, `src/pbrt/ray.h`).

use umbra_math::{Point3f, Vector3f};

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
}
