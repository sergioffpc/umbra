//! The bilinear distribution on `[0, 1]²`, which Triangles use to weight
//! solid-angle samples by the cosine at the reference point.

use umbra_math::{Point2f, lerp};

use super::univariate::{invert_linear_sample, sample_linear};

/// The PDF on `[0, 1]²` of the bilinear interpolation of the corner values
/// `w = [w(0,0), w(1,0), w(0,1), w(1,1)]`; 1 if they are all zero, 0 outside
/// the square.
///
/// PBR 4e §A.5.6, pbrt-v4 `BilinearPDF`.
#[must_use]
pub fn bilinear_pdf(p: Point2f, w: [f32; 4]) -> f32 {
    if p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0 {
        return 0.0;
    }
    let sum = w[0] + w[1] + w[2] + w[3];
    if sum == 0.0 {
        return 1.0;
    }
    4.0 * ((1.0 - p.x) * (1.0 - p.y) * w[0]
        + p.x * (1.0 - p.y) * w[1]
        + (1.0 - p.x) * p.y * w[2]
        + p.x * p.y * w[3])
        / sum
}

/// A sample of [`bilinear_pdf`]: `y` from the marginal, then `x` from the
/// conditional, both linear.
///
/// PBR 4e §A.5.6, pbrt-v4 `SampleBilinear`.
#[must_use]
pub fn sample_bilinear(u: Point2f, w: [f32; 4]) -> Point2f {
    let y = sample_linear(u.y, w[0] + w[1], w[2] + w[3]);
    let x = sample_linear(u.x, lerp(y, w[0], w[2]), lerp(y, w[1], w[3]));
    Point2f::new(x, y)
}

/// The `u` that [`sample_bilinear`] maps to `p`.
///
/// pbrt-v4 `InvertBilinearSample`.
#[must_use]
pub fn invert_bilinear_sample(p: Point2f, w: [f32; 4]) -> Point2f {
    Point2f::new(
        invert_linear_sample(p.x, lerp(p.y, w[0], w[2]), lerp(p.y, w[1], w[3])),
        invert_linear_sample(p.y, w[0] + w[1], w[2] + w[3]),
    )
}
