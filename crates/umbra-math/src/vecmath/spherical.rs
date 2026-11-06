//! Spherical geometry of unit directions, in the shading coordinate system
//! where the normal is `+z` (pbrt-v4 `src/pbrt/util/vecmath.h`).

use crate::PI;
use crate::math::{clamp, max, safe_acos, sqr};
use crate::vecmath::vector::{Vector3f, angle_between, cross, dot};

/// The area of the spherical triangle with unit-vector vertices `a`, `b`,
/// `c` (Van Oosterom and Strackee).
///
/// PBR 4e §3.8.1, pbrt-v4 `SphericalTriangleArea`.
#[must_use]
pub fn spherical_triangle_area(a: Vector3f, b: Vector3f, c: Vector3f) -> f32 {
    (2.0 * libm::atan2f(dot(a, cross(b, c)), 1.0 + dot(a, b) + dot(a, c) + dot(b, c))).abs()
}

/// The area of the spherical quadrilateral with unit-vector vertices `a`,
/// `b`, `c`, `d`, from its interior angles; zero if it is degenerate.
///
/// PBR 4e §3.8.1, pbrt-v4 `SphericalQuadArea`.
#[must_use]
pub fn spherical_quad_area(a: Vector3f, b: Vector3f, c: Vector3f, d: Vector3f) -> f32 {
    let edges = [cross(a, b), cross(b, c), cross(c, d), cross(d, a)];
    if edges.iter().any(|e| e.length_squared() == 0.0) {
        return 0.0;
    }
    let [axb, bxc, cxd, dxa] = edges.map(Vector3f::normalize);
    let alpha = angle_between(dxa, -axb);
    let beta = angle_between(axb, -bxc);
    let gamma = angle_between(bxc, -cxd);
    let delta = angle_between(cxd, -dxa);
    (alpha + beta + gamma + delta - 2.0 * PI).abs()
}

/// The unit direction with polar angle given by its sine and cosine, and
/// azimuth `phi`.
///
/// PBR 4e §3.8.2, pbrt-v4 `SphericalDirection`.
#[must_use]
pub fn spherical_direction(sin_theta: f32, cos_theta: f32, phi: f32) -> Vector3f {
    debug_assert!(
        (-1.0001..=1.0001).contains(&sin_theta),
        "sin θ = {sin_theta}"
    );
    debug_assert!(
        (-1.0001..=1.0001).contains(&cos_theta),
        "cos θ = {cos_theta}"
    );
    let sin_theta = clamp(sin_theta, -1.0, 1.0);
    Vector3f::new(
        sin_theta * libm::cosf(phi),
        sin_theta * libm::sinf(phi),
        clamp(cos_theta, -1.0, 1.0),
    )
}

/// The polar angle θ of the unit vector `v`.
///
/// PBR 4e §3.8.2, pbrt-v4 `SphericalTheta`.
#[must_use]
pub fn spherical_theta(v: Vector3f) -> f32 {
    safe_acos(v.z)
}

/// The azimuth φ of `v`, in `[0, 2π)`.
///
/// PBR 4e §3.8.2, pbrt-v4 `SphericalPhi`.
#[must_use]
pub fn spherical_phi(v: Vector3f) -> f32 {
    let p = libm::atan2f(v.y, v.x);
    if p < 0.0 { p + 2.0 * PI } else { p }
}

/// PBR 4e §3.8.3, pbrt-v4 `CosTheta`.
#[inline]
#[must_use]
pub fn cos_theta(w: Vector3f) -> f32 {
    w.z
}

/// PBR 4e §3.8.3, pbrt-v4 `Cos2Theta`.
#[inline]
#[must_use]
pub fn cos2_theta(w: Vector3f) -> f32 {
    sqr(w.z)
}

/// PBR 4e §3.8.3, pbrt-v4 `AbsCosTheta`.
#[inline]
#[must_use]
pub fn abs_cos_theta(w: Vector3f) -> f32 {
    w.z.abs()
}

/// PBR 4e §3.8.3, pbrt-v4 `Sin2Theta`.
#[inline]
#[must_use]
pub fn sin2_theta(w: Vector3f) -> f32 {
    max(0.0, 1.0 - cos2_theta(w))
}

/// PBR 4e §3.8.3, pbrt-v4 `SinTheta`.
#[inline]
#[must_use]
pub fn sin_theta(w: Vector3f) -> f32 {
    libm::sqrtf(sin2_theta(w))
}

/// PBR 4e §3.8.3, pbrt-v4 `TanTheta`.
#[inline]
#[must_use]
pub fn tan_theta(w: Vector3f) -> f32 {
    sin_theta(w) / cos_theta(w)
}

/// PBR 4e §3.8.3, pbrt-v4 `Tan2Theta`.
#[inline]
#[must_use]
pub fn tan2_theta(w: Vector3f) -> f32 {
    sin2_theta(w) / cos2_theta(w)
}

/// `cos φ`, taken as 1 at the poles.
///
/// PBR 4e §3.8.3, pbrt-v4 `CosPhi`.
#[must_use]
pub fn cos_phi(w: Vector3f) -> f32 {
    let sin_theta = sin_theta(w);
    if sin_theta == 0.0 {
        1.0
    } else {
        clamp(w.x / sin_theta, -1.0, 1.0)
    }
}

/// `sin φ`, taken as 0 at the poles.
///
/// PBR 4e §3.8.3, pbrt-v4 `SinPhi`.
#[must_use]
pub fn sin_phi(w: Vector3f) -> f32 {
    let sin_theta = sin_theta(w);
    if sin_theta == 0.0 {
        0.0
    } else {
        clamp(w.y / sin_theta, -1.0, 1.0)
    }
}

/// The cosine of the azimuth difference of `wa` and `wb`.
///
/// PBR 4e §3.8.3, pbrt-v4 `CosDPhi`.
#[must_use]
#[expect(clippy::similar_names, reason = "pbrt-v4's `waxy` and `wbxy`")]
pub fn cos_d_phi(wa: Vector3f, wb: Vector3f) -> f32 {
    let waxy = sqr(wa.x) + sqr(wa.y);
    let wbxy = sqr(wb.x) + sqr(wb.y);
    if waxy == 0.0 || wbxy == 0.0 {
        return 1.0;
    }
    clamp(
        (wa.x * wb.x + wa.y * wb.y) / libm::sqrtf(waxy * wbxy),
        -1.0,
        1.0,
    )
}

/// Whether `w` and `wp` are on the same side of the `z = 0` plane. `wp` may
/// be a vector or a normal.
///
/// PBR 4e §9.1, pbrt-v4 `SameHemisphere`.
#[must_use]
pub fn same_hemisphere(w: Vector3f, wp: impl Into<Vector3f>) -> bool {
    w.z * wp.into().z > 0.0
}
