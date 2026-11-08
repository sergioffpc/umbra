//! The Henyey–Greenstein phase function, which the medium between the layers
//! of a layered BxDF scatters by, and its sampling.

use umbra_math::{
    Frame, INV_4_PI, PI, Point2f, Vector3f, clamp, safe_sqrt, spherical_direction, sqr,
};

/// pbrt-v4 keeps `g` inside `[−0.99, 0.99]`, where the phase function and
/// its sampling stay numerically stable.
const MAX_ASYMMETRY: f32 = 0.99;

/// The Henyey–Greenstein phase function with asymmetry `g` (clamped to
/// `[−0.99, 0.99]`) at the cosine `cos_theta` of the angle between `wo` and
/// `wi`, both pointing away from the scattering point: forward scattering
/// (`g > 0`) peaks at `cos θ = −1`. It integrates to 1 over the sphere.
///
/// PBR 4e §11.3.1, pbrt-v4 `HenyeyGreenstein`.
#[must_use]
pub fn henyey_greenstein(cos_theta: f32, g: f32) -> f32 {
    let g = clamp(g, -MAX_ASYMMETRY, MAX_ASYMMETRY);
    let denom = 1.0 + sqr(g) + 2.0 * g * cos_theta;
    INV_4_PI * (1.0 - sqr(g)) / (denom * safe_sqrt(denom))
}

/// A direction drawn from [`sample_henyey_greenstein`] and its PDF with
/// respect to solid angle, the phase function's value there.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct HenyeyGreensteinSample {
    /// The sampled direction, pointing away from the scattering point.
    pub wi: Vector3f,
    /// [`henyey_greenstein`] at the sampled direction.
    pub pdf: f32,
}

/// A direction `wi` distributed as [`henyey_greenstein`] with asymmetry `g`
/// (clamped to `[−0.99, 0.99]`) for the outgoing direction `wo`, by inverting
/// its CDF in `cos θ`.
///
/// PBR 4e §11.3.1, pbrt-v4 `SampleHenyeyGreenstein`.
#[must_use]
pub fn sample_henyey_greenstein(wo: Vector3f, g: f32, u: Point2f) -> HenyeyGreensteinSample {
    let g = clamp(g, -MAX_ASYMMETRY, MAX_ASYMMETRY);
    let cos_theta = if g.abs() < 1e-3 {
        1.0 - 2.0 * u.x
    } else {
        -1.0 / (2.0 * g) * (1.0 + sqr(g) - sqr((1.0 - sqr(g)) / (1.0 + g - 2.0 * g * u.x)))
    };

    let sin_theta = safe_sqrt(1.0 - sqr(cos_theta));
    let phi = 2.0 * PI * u.y;
    let w_frame = Frame::from_z(wo);
    let wi = w_frame.from_local(spherical_direction(sin_theta, cos_theta, phi));
    HenyeyGreensteinSample {
        wi,
        pdf: henyey_greenstein(cos_theta, g),
    }
}
