//! Directions on the unit sphere: uniform over the sphere, the hemisphere and
//! a cone, and cosine-weighted over the hemisphere. Hemispheres and cones are
//! around `+z`.

use umbra_math::{
    INV_2_PI, INV_4_PI, INV_PI, PI, Point2f, Vector3f, safe_sqrt, spherical_direction,
    spherical_phi, sqr,
};

use super::disk::{invert_uniform_disk_concentric_sample, sample_uniform_disk_concentric};

/// A uniformly distributed direction of the unit sphere.
///
/// PBR 4e §A.5.2, pbrt-v4 `SampleUniformSphere`.
#[must_use]
pub fn sample_uniform_sphere(u: Point2f) -> Vector3f {
    let z = 1.0 - 2.0 * u.x;
    let r = safe_sqrt(1.0 - sqr(z));
    let phi = 2.0 * PI * u.y;
    Vector3f::new(r * libm::cosf(phi), r * libm::sinf(phi), z)
}

/// The PDF of [`sample_uniform_sphere`], `1 / 4π`, with respect to solid
/// angle.
///
/// PBR 4e §A.5.2, pbrt-v4 `UniformSpherePDF`.
#[must_use]
pub const fn uniform_sphere_pdf() -> f32 {
    INV_4_PI
}

/// The `u` that [`sample_uniform_sphere`] maps to `w`.
///
/// pbrt-v4 `InvertUniformSphereSample`.
#[must_use]
pub fn invert_uniform_sphere_sample(w: Vector3f) -> Point2f {
    Point2f::new((1.0 - w.z) / 2.0, spherical_phi(w) / (2.0 * PI))
}

/// A uniformly distributed direction of the hemisphere `z ≥ 0`.
///
/// PBR 4e §A.5.2, pbrt-v4 `SampleUniformHemisphere`.
#[must_use]
pub fn sample_uniform_hemisphere(u: Point2f) -> Vector3f {
    let z = u.x;
    let r = safe_sqrt(1.0 - sqr(z));
    let phi = 2.0 * PI * u.y;
    Vector3f::new(r * libm::cosf(phi), r * libm::sinf(phi), z)
}

/// The PDF of [`sample_uniform_hemisphere`], `1 / 2π`, with respect to solid
/// angle.
///
/// PBR 4e §A.5.2, pbrt-v4 `UniformHemispherePDF`.
#[must_use]
pub const fn uniform_hemisphere_pdf() -> f32 {
    INV_2_PI
}

/// The `u` that [`sample_uniform_hemisphere`] maps to `w`.
///
/// pbrt-v4 `InvertUniformHemisphereSample`.
#[must_use]
pub fn invert_uniform_hemisphere_sample(w: Vector3f) -> Point2f {
    Point2f::new(w.z, spherical_phi(w) / (2.0 * PI))
}

/// A direction of the hemisphere `z ≥ 0` distributed as `cos θ`, by
/// Malley's method: a uniform point of the disk projected up.
///
/// PBR 4e §A.5.3, pbrt-v4 `SampleCosineHemisphere`.
#[must_use]
pub fn sample_cosine_hemisphere(u: Point2f) -> Vector3f {
    let d = sample_uniform_disk_concentric(u);
    let z = safe_sqrt(1.0 - sqr(d.x) - sqr(d.y));
    Vector3f::new(d.x, d.y, z)
}

/// The PDF of [`sample_cosine_hemisphere`] at a direction with polar angle
/// cosine `cos_theta`, `cos θ / π`, with respect to solid angle.
///
/// PBR 4e §A.5.3, pbrt-v4 `CosineHemispherePDF`.
#[must_use]
pub fn cosine_hemisphere_pdf(cos_theta: f32) -> f32 {
    cos_theta * INV_PI
}

/// The `u` that [`sample_cosine_hemisphere`] maps to `w`.
///
/// pbrt-v4 `InvertCosineHemisphereSample`.
#[must_use]
pub fn invert_cosine_hemisphere_sample(w: Vector3f) -> Point2f {
    invert_uniform_disk_concentric_sample(Point2f::new(w.x, w.y))
}

/// A uniformly distributed direction of the cone around `+z` whose half
/// angle has cosine `cos_theta_max`.
///
/// PBR 4e §A.5.4, pbrt-v4 `SampleUniformCone`.
#[must_use]
pub fn sample_uniform_cone(u: Point2f, cos_theta_max: f32) -> Vector3f {
    let cos_theta = (1.0 - u.x) + u.x * cos_theta_max;
    let sin_theta = safe_sqrt(1.0 - sqr(cos_theta));
    let phi = u.y * 2.0 * PI;
    spherical_direction(sin_theta, cos_theta, phi)
}

/// The PDF of [`sample_uniform_cone`], `1 / 2π(1 − cos θmax)`, with respect
/// to solid angle.
///
/// PBR 4e §A.5.4, pbrt-v4 `UniformConePDF`.
#[must_use]
pub fn uniform_cone_pdf(cos_theta_max: f32) -> f32 {
    1.0 / (2.0 * PI * (1.0 - cos_theta_max))
}

/// The `u` that [`sample_uniform_cone`] maps to `w`.
///
/// pbrt-v4 `InvertUniformConeSample`.
#[must_use]
pub fn invert_uniform_cone_sample(w: Vector3f, cos_theta_max: f32) -> Point2f {
    let cos_theta = w.z;
    let phi = spherical_phi(w);
    Point2f::new((cos_theta - 1.0) / (cos_theta_max - 1.0), phi / (2.0 * PI))
}
