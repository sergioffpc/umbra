//! The scalar IEEE primitives as the hardware instruction of the target
//! (ADR 0007): correctly rounded [`fma`] and [`sqrt`], exact [`floor`] and
//! [`ceil`], and the `f64` [`fma_f64`] that Umbra's own transcendentals
//! evaluate their polynomials with (ADR 0022).
//!
//! `libm` computes the same bits, but on x86-64 its `fmaf` is an indirect
//! call through a runtime CPU check and its `sqrtf` is opaque inline
//! assembly, so LLVM can neither inline, schedule nor vectorise them; here
//! each is one instruction that LLVM sees through, with the same bits
//! (ADR 0022). `tests/umbra_math/reference/float.rs` keeps the `libm` calls
//! as the test oracle. The crate supports x86-64 only (`lib.rs`, ADR 0023),
//! so there is no fallback.
//!
//! The intrinsics are called in functions marked with the `target_feature`
//! they need. The crate requires `x86-64-v3` (FMA, SSE4.1, AVX2) for the
//! whole build and refuses to compile without it (`lib.rs`), so calling them
//! is sound on every CPU the binary can run on.

#[cfg(target_arch = "x86_64")]
mod x86_64 {
    use core::arch::x86_64::{
        _mm_ceil_ss, _mm_cvtsd_f64, _mm_cvtss_f32, _mm_floor_ss, _mm_fmadd_sd, _mm_fmadd_ss,
        _mm_set_sd, _mm_set_ss, _mm_sqrt_ss,
    };

    /// `sqrtss`.
    #[target_feature(enable = "sse2")]
    #[inline]
    pub(super) fn sqrt(x: f32) -> f32 {
        _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)))
    }

    /// `vfmadd213ss`.
    #[target_feature(enable = "fma")]
    #[inline]
    pub(super) fn fma(a: f32, b: f32, c: f32) -> f32 {
        _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(a), _mm_set_ss(b), _mm_set_ss(c)))
    }

    /// `vfmadd213sd`.
    #[target_feature(enable = "fma")]
    #[inline]
    pub(super) fn fma_f64(a: f64, b: f64, c: f64) -> f64 {
        _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(b), _mm_set_sd(c)))
    }

    /// `roundss` towards −∞.
    #[target_feature(enable = "sse4.1")]
    #[inline]
    pub(super) fn floor(x: f32) -> f32 {
        let v = _mm_set_ss(x);
        _mm_cvtss_f32(_mm_floor_ss(v, v))
    }

    /// `roundss` towards +∞.
    #[target_feature(enable = "sse4.1")]
    #[inline]
    pub(super) fn ceil(x: f32) -> f32 {
        let v = _mm_set_ss(x);
        _mm_cvtss_f32(_mm_ceil_ss(v, v))
    }
}

#[cfg(target_arch = "x86_64")]
use x86_64 as hardware;

/// Defines the crate primitive `$name` as the instruction of
/// `hardware::$name`.
macro_rules! primitive {
    ($(#[$doc:meta])* $name:ident($($arg:ident),+)) => {
        $(#[$doc])*
        #[inline]
        #[must_use]
        #[expect(
            unsafe_code,
            reason = "aggressive optimisation (ADR 0007): the hardware instruction"
        )]
        pub(crate) fn $name($($arg: f32),+) -> f32 {
            // SAFETY: the instruction's target feature is enabled for the
            // whole build: `x86-64-v3` is required (`lib.rs`), so every CPU
            // this binary runs on has it.
            unsafe { hardware::$name($($arg),+) }
        }
    };
}

primitive! {
    /// `a · b + c` with one rounding.
    fma(a, b, c)
}

/// `a · b + c` in `f64`, with one rounding.
#[inline]
#[must_use]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): the hardware instruction"
)]
pub(crate) fn fma_f64(a: f64, b: f64, c: f64) -> f64 {
    // SAFETY: the instruction's target feature is enabled for the whole
    // build: `x86-64-v3` is required (`lib.rs`), so every CPU this binary
    // runs on has it.
    unsafe { hardware::fma_f64(a, b, c) }
}

primitive! {
    /// `√x`, correctly rounded.
    sqrt(x)
}

primitive! {
    /// `⌊x⌋`.
    floor(x)
}

primitive! {
    /// `⌈x⌉`.
    ceil(x)
}
