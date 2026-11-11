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
//! The names and signatures are C's, so `sinf` is the `f32` sine and `sin`
//! the `f64` one.

#[cfg(target_arch = "x86_64")]
mod x86_64;

#[cfg(target_arch = "x86_64")]
pub use x86_64::*;
