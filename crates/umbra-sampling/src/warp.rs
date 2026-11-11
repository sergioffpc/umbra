//! Sampling functions, or warps (pbrt-v4 `src/pbrt/util/sampling.h` and
//! `sampling.cpp`): maps from uniform samples in `[0, 1)ⁿ` to the
//! distributions Shapes, Lights, BxDFs, Cameras and Films sample, their PDFs
//! and, where pbrt-v4 has them, their inverses.
//!
//! Each follows pbrt-v4's operations in their order, with no implicit FMA
//! (ADR 0001), so it computes pbrt-v4's bits up to the last-bit differences
//! between the glibc of Umbra's image and the libm pbrt-v4 was built with.

mod bilinear;
mod discrete;
mod disk;
mod henyey_greenstein;
mod sphere;
mod triangle;
mod univariate;

pub use bilinear::{bilinear_pdf, invert_bilinear_sample, sample_bilinear};
pub use discrete::{DiscreteSample, balance_heuristic, power_heuristic, sample_discrete};
pub use disk::{
    invert_uniform_disk_concentric_sample, invert_uniform_disk_polar_sample,
    sample_uniform_disk_concentric, sample_uniform_disk_polar,
};
pub use henyey_greenstein::{HenyeyGreensteinSample, henyey_greenstein, sample_henyey_greenstein};
pub use sphere::{
    cosine_hemisphere_pdf, invert_cosine_hemisphere_sample, invert_uniform_cone_sample,
    invert_uniform_hemisphere_sample, invert_uniform_sphere_sample, sample_cosine_hemisphere,
    sample_uniform_cone, sample_uniform_hemisphere, sample_uniform_sphere, uniform_cone_pdf,
    uniform_hemisphere_pdf, uniform_sphere_pdf,
};
pub use triangle::{
    SphericalTriangleSample, invert_spherical_triangle_sample, invert_uniform_triangle_sample,
    sample_spherical_triangle, sample_uniform_triangle,
};
pub use univariate::{
    exponential_pdf, invert_exponential_sample, invert_linear_sample, linear_pdf,
    sample_exponential, sample_linear, sample_visible_wavelengths, visible_wavelengths_pdf,
};
