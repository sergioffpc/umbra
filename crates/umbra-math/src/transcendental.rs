//! The scalar transcendentals [`sin_cos`], [`atan2`] and [`erf`], one backend
//! per architecture.
//!
//! - `cpu`, on x86-64 and arm64: Umbra's own, evaluated in `f64` and within
//!   1 ULP of the correctly rounded result (ADR 0022). `f64` lives in this
//!   backend and in the other CPU kernels only (ADR 0028).
//! - `nvptx64`: `sin_cos` by PTX's approximate instructions after an
//!   argument reduction of Umbra's own, `atan2` and `erf` from CUDA's
//!   `libdevice`, each with its documented error bound. The GPU promises
//!   statistical equality only (ADR 0026), so the results are not the
//!   CPU's bits.

#[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
#[expect(
    clippy::disallowed_types,
    reason = "the CPU backend evaluates in f64 (ADR 0028)"
)]
mod cpu;
#[cfg(target_arch = "nvptx64")]
mod nvptx64;

#[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
pub use cpu::{atan2, erf, sin_cos};
#[cfg(target_arch = "nvptx64")]
pub use nvptx64::{atan2, erf, sin_cos};
