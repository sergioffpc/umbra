//! The scalar IEEE primitives as the hardware instruction of the target
//! (ADR 0007): correctly rounded [`fma`] and [`sqrt`], exact [`floor`] and
//! [`ceil`].
//!
//! `libm` computes the same bits, but on x86-64 its `fmaf` is an indirect
//! call through a runtime CPU check and its `sqrtf` is opaque inline
//! assembly, so LLVM can neither inline, schedule nor vectorise them; here
//! each is one instruction that LLVM sees through, with the same bits
//! (ADR 0002). `tests/umbra_math/reference/float.rs` keeps the `libm` calls
//! as the test oracle. The crate supports x86-64 and arm64 with NEON only
//! (`lib.rs`), so there is no fallback.
//!
//! The intrinsics are called in functions marked with the `target_feature`
//! they need. On x86-64 the crate requires `x86-64-v3` (FMA, SSE4.1, AVX2)
//! for the whole build and refuses to compile without it (`lib.rs`), and on
//! arm64 NEON is part of the target, so calling them is sound on every CPU
//! the binary can run on.

#[cfg(target_arch = "x86_64")]
mod x86_64 {
    use core::arch::x86_64::{
        _mm_ceil_ss, _mm_cvtss_f32, _mm_floor_ss, _mm_fmadd_ss, _mm_set_ss, _mm_sqrt_ss,
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

#[cfg(all(target_arch = "aarch64", target_feature = "neon"))]
mod aarch64 {
    use core::arch::aarch64::{
        vdup_n_f32, vfma_f32, vget_lane_f32, vrndm_f32, vrndp_f32, vsqrt_f32,
    };

    /// `fsqrt`.
    #[target_feature(enable = "neon")]
    #[inline]
    pub(super) fn sqrt(x: f32) -> f32 {
        vget_lane_f32::<0>(vsqrt_f32(vdup_n_f32(x)))
    }

    /// `fmadd`.
    #[target_feature(enable = "neon")]
    #[inline]
    pub(super) fn fma(a: f32, b: f32, c: f32) -> f32 {
        vget_lane_f32::<0>(vfma_f32(vdup_n_f32(c), vdup_n_f32(a), vdup_n_f32(b)))
    }

    /// `frintm`.
    #[target_feature(enable = "neon")]
    #[inline]
    pub(super) fn floor(x: f32) -> f32 {
        vget_lane_f32::<0>(vrndm_f32(vdup_n_f32(x)))
    }

    /// `frintp`.
    #[target_feature(enable = "neon")]
    #[inline]
    pub(super) fn ceil(x: f32) -> f32 {
        vget_lane_f32::<0>(vrndp_f32(vdup_n_f32(x)))
    }
}

#[cfg(target_arch = "x86_64")]
use x86_64 as hardware;

#[cfg(all(target_arch = "aarch64", target_feature = "neon"))]
use aarch64 as hardware;

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
            // whole build: `x86-64-v3` is required on x86-64 and NEON on
            // arm64 (`lib.rs`), so every CPU this binary runs on has it.
            unsafe { hardware::$name($($arg),+) }
        }
    };
}

primitive! {
    /// `a · b + c` with one rounding.
    fma(a, b, c)
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
