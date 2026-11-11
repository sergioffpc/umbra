//! The pbrt-v4 sampling routines the ported tests draw directions with
//! (`src/pbrt/util/sampling.h`). Test support only: `umbra-sampling` owns them.

use umbra_math::cmath;
use umbra_math::{PI, Point2f, Vector3f, safe_sqrt, sqr};

/// pbrt-v4 `SampleUniformSphere`.
pub(crate) fn sample_uniform_sphere(u: Point2f) -> Vector3f {
    let z = 1.0 - 2.0 * u.x;
    let r = safe_sqrt(1.0 - sqr(z));
    let phi = 2.0 * PI * u.y;
    Vector3f::new(r * cmath::cosf(phi), r * cmath::sinf(phi), z)
}

/// pbrt-v4 `Hammersley2D(n)`: `(i / n, RadicalInverse(0, i))`, the base-2
/// radical inverse being the reversed bits of `i`.
pub(crate) fn hammersley_2d(n: u16) -> impl Iterator<Item = Point2f> {
    (0..n).map(move |i| {
        let radical_inverse = f32::from_bits(0x2f80_0000) * reversed_bits(i);
        Point2f::new(f32::from(i) / f32::from(n), radical_inverse)
    })
}

/// `i` with its 32 bits reversed, exact in `f32` since `i` has 16 bits.
fn reversed_bits(i: u16) -> f32 {
    f32::from((u32::from(i).reverse_bits() >> 16) as u16) * 65536.0
}
