//! The `f64` C math functions, one safe wrapper each over the platform's
//! math library (ADR 0007, ADR 0022): glibc's `libm.so.6` on x86-64.
//!
//! The renderer works in `f32`, whose wrappers are at the crate root
//! (`umbra_math::tanf`); these serve the `f64` test oracles and
//! `umbra-spectrum`'s build script. They keep C's names, which would collide
//! at the crate root with Umbra's own `f32` [`atan2`](crate::atan2) and
//! [`erf`](crate::erf).

pub use crate::platform::{
    acos, atan2, cbrt, cos, erf, exp, fma, hypot, lgamma, log, pow, sin, sqrt,
};
