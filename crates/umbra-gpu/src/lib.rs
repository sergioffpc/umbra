#![doc = include_str!("../README.md")]

mod error;
// The layout shared with the device: one file, compiled by both crates.
#[path = "../../umbra-kernels/src/launch.rs"]
mod launch;
mod optix;
mod records;
mod tracer;

pub use error::Error;
pub use tracer::Tracer;

/// The geometry a [`Tracer`] traces rays against.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Geometry {
    /// The triangle, by OptiX's built-in intersection.
    Triangle,
    /// The sphere, by the custom intersection program, which calls the
    /// core's `Sphere::intersect`.
    Sphere,
}

/// A function of `umbra-math` that [`Tracer::evaluate`] evaluates on the
/// GPU, where it is the `nvptx64` backend's: CUDA's `libdevice`, or
/// Umbra's own kernel built on it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Function {
    /// `umbra_math::fma(x, y, z)`: `__nv_fmaf`.
    Fma,
    /// `umbra_math::sqrt(x)`: `__nv_fsqrt_rn`.
    Sqrt,
    /// `umbra_math::floor(x)`: `__nv_floorf`.
    Floor,
    /// `umbra_math::ceil(x)`: `__nv_ceilf`.
    Ceil,
    /// `umbra_math::roundf(x)`: `__nv_roundf`.
    Roundf,
    /// `umbra_math::sinf(x)`: `__nv_sinf`.
    Sinf,
    /// The sine of `umbra_math::sin_cos(x)`: `__nv_fast_sinf` after
    /// Umbra's argument reduction for `|x| ≤ 2²⁰`, `__nv_sinf` beyond.
    SinOfSinCos,
    /// The cosine of `umbra_math::sin_cos(x)`: `__nv_fast_cosf` after
    /// Umbra's argument reduction for `|x| ≤ 2²⁰`, `__nv_cosf` beyond.
    CosOfSinCos,
    /// `umbra_math::tanf(x)`: `__nv_tanf`.
    Tanf,
    /// `umbra_math::asinf(x)`: `__nv_asinf`.
    Asinf,
    /// `umbra_math::acosf(x)`: `__nv_acosf`.
    Acosf,
    /// `umbra_math::atan2(y, x)`, of the arguments `[y, x, _]`:
    /// `__nv_atan2f`.
    Atan2,
    /// `umbra_math::erf(x)`: `__nv_erff`.
    Erf,
    /// `umbra_math::expf(x)`: `__nv_expf`.
    Expf,
    /// `umbra_math::powf(x, y)`: `__nv_powf`.
    Powf,
    /// `umbra_math::logf(x)`: `__nv_logf`.
    Logf,
    /// `umbra_math::atanhf(x)`: `__nv_atanhf`.
    Atanhf,
    /// `umbra_math::coshf(x)`: `__nv_coshf`.
    Coshf,
}

/// Where a ray first hit a [`Geometry`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Hit {
    /// The ray parameter of the hit.
    pub t: f32,
    /// What it hit.
    pub geometry: Geometry,
}
