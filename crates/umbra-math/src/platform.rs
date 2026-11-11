//! The C math functions Umbra takes from the platform's math library, one
//! safe wrapper per function (ADR 0007, ADR 0022): glibc's `libm.so.6` on
//! x86-64.
//!
//! Umbra's own kernels come first where they exist: [`sin_cos`](crate::sin_cos),
//! [`atan2`](crate::atan2), [`erf`](crate::erf), [`Vec4::exp`](crate::Vec4::exp)
//! and [`Vec4::pow`](crate::Vec4::pow). These wrappers serve everything else:
//! the cold functions (`asinf`, `acosf`, `tanf`, `coshf`, `atanhf`, `logf`,
//! …), the kernels' own fallbacks, and the `f64` oracles of the tests. Each
//! has the accuracy that glibc's manual states for it ("Known Maximum Errors
//! in Math Functions"); glibc's bits are fixed per release, since every
//! Worker and the Coordinator run one image (ADR 0025).
//!
//! The names and signatures are C's. The `f32` wrappers are public at the
//! crate root (`umbra_math::tanf`); the `f64` ones in
//! [`double`](crate::double), where `sin` and `atan2` do not collide with
//! Umbra's own `f32` kernels.

#[cfg(target_arch = "x86_64")]
#[expect(
    clippy::disallowed_types,
    reason = "glibc's f64 functions, on x86-64 only (ADR 0028)"
)]
mod x86_64;

#[cfg(target_arch = "x86_64")]
pub use x86_64::{
    acos, acosf, asinf, atan2, atan2f, atanhf, cbrt, ceilf, cos, cosf, coshf, erf, erff, exp,
    exp2f, expf, floorf, fma, fmaf, hypot, lgamma, log, logf, pow, powf, roundf, sin, sincosf,
    sinf, sqrt, sqrtf, tanf,
};

// On `nvptx64` the crate's own calls take `libdevice`'s wrappers where it
// has the function (ADR 0028); they stay crate-private until #146 gives the
// GPU the full set. `acosf`, `asinf`, `logf`, `tanf` and `roundf` have no
// `libdevice` wrapper yet, so the `nvptx64` build lacks them.
#[cfg(target_arch = "nvptx64")]
pub(crate) use crate::libdevice::{powf, sinf};
