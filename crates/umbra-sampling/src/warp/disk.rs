//! Uniform samples of the unit disk.

use umbra_math::{PI, PI_OVER_2, PI_OVER_4, Point2f, Vector2f, atan2, sin_cos, sqr, sqrt};

/// A uniformly distributed point of the unit disk, by the polar map
/// `r = √u₀, θ = 2πu₁`, which distorts areas near the centre.
///
/// PBR 4e §A.5.1, pbrt-v4 `SampleUniformDiskPolar`.
#[must_use]
pub fn sample_uniform_disk_polar(u: Point2f) -> Point2f {
    let r = sqrt(u.x);
    let (sin, cos) = sin_cos(2.0 * PI * u.y);
    Point2f::new(r * cos, r * sin)
}

/// The `u` that [`sample_uniform_disk_polar`] maps to `p`.
///
/// pbrt-v4 `InvertUniformDiskPolarSample`.
#[must_use]
pub fn invert_uniform_disk_polar_sample(p: Point2f) -> Point2f {
    let mut phi = atan2(p.y, p.x);
    if phi < 0.0 {
        phi += 2.0 * PI;
    }
    Point2f::new(sqr(p.x) + sqr(p.y), phi / (2.0 * PI))
}

/// A uniformly distributed point of the unit disk, by Shirley and Chiu's
/// concentric map of squares to circles, which keeps strata compact.
///
/// PBR 4e §A.5.1, pbrt-v4 `SampleUniformDiskConcentric`.
#[must_use]
pub fn sample_uniform_disk_concentric(u: Point2f) -> Point2f {
    let u_offset = 2.0 * u - Vector2f::new(1.0, 1.0);
    if u_offset.x == 0.0 && u_offset.y == 0.0 {
        return Point2f::new(0.0, 0.0);
    }
    let (r, theta) = if u_offset.x.abs() > u_offset.y.abs() {
        (u_offset.x, PI_OVER_4 * (u_offset.y / u_offset.x))
    } else {
        (
            u_offset.y,
            PI_OVER_2 - PI_OVER_4 * (u_offset.x / u_offset.y),
        )
    };
    let (sin, cos) = sin_cos(theta);
    r * Point2f::new(cos, sin)
}

/// The `u` that [`sample_uniform_disk_concentric`] maps to `p`.
///
/// pbrt-v4 `InvertUniformDiskConcentricSample`.
#[must_use]
pub fn invert_uniform_disk_concentric_sample(p: Point2f) -> Point2f {
    let theta = atan2(p.y, p.x);
    let r = sqrt(sqr(p.x) + sqr(p.y));
    // The comparisons are pbrt-v4's, so a NaN takes the same branch.
    let uo = if theta.abs() < PI_OVER_4 || theta.abs() > 3.0 * PI_OVER_4 {
        let r = r.copysign(p.x);
        let y = match (p.x < 0.0, p.y < 0.0) {
            (true, true) => (PI + theta) * r / PI_OVER_4,
            (true, false) => (theta - PI) * r / PI_OVER_4,
            (false, _) => (theta * r) / PI_OVER_4,
        };
        Point2f::new(r, y)
    } else {
        let r = r.copysign(p.y);
        let x = if p.y < 0.0 {
            -(PI_OVER_2 + theta) * r / PI_OVER_4
        } else {
            (PI_OVER_2 - theta) * r / PI_OVER_4
        };
        Point2f::new(x, r)
    };
    Point2f::new(to_unit(uo.x), to_unit(uo.y))
}

/// `[−1, 1]` to `[0, 1]`, as pbrt-v4 writes it.
#[expect(clippy::manual_midpoint, reason = "pbrt-v4's (x + 1) / 2")]
fn to_unit(x: f32) -> f32 {
    (x + 1.0) / 2.0
}
