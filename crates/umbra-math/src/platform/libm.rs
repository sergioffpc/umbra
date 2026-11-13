//! glibc's scalar math, `libm.so.6`, through FFI (ADR 0007): each function
//! declared once, in `ffi`, and called only by its safe wrapper here, one
//! call per `unsafe` block.
//!
//! The file depends on nothing but `core`, so `umbra-spectrum`'s build script
//! compiles it by `#[path]` too: the table it generates takes its cube root
//! from the same wrapper, without the x86-64-v3 requirement of the crate.

/// Declares every C function in `ffi`, and defines the safe wrapper, of the
/// same name and signature, of each one listed after the `extern` block. The
/// functions in the `extern` block write through pointers, so their
/// wrappers, written by hand below, differ from the C signature.
macro_rules! wrappers {
    (
        extern {
            $($(#[$out_attr:meta])* fn $out:ident($($out_arg:ident: $out_ty:ty),+) $(-> $out_ret:ty)?;)+
        }
        $($(#[$doc:meta])* fn $name:ident($($arg:ident: $ty:ty),+) -> $ret:ty;)+
    ) => {
        /// The declarations: only the wrappers call them.
        mod ffi {
            #[link(name = "m")]
            #[expect(unsafe_code, reason = "glibc's scalar math (ADR 0007)")]
            unsafe extern "C" {
                $($(#[$out_attr])* pub(super) fn $out($($out_arg: $out_ty),+) $(-> $out_ret)?;)+
                $(pub(super) fn $name($($arg: $ty),+) -> $ret;)+
            }
        }

        $(
            $(#[$doc])*
            #[inline]
            #[must_use]
            #[expect(unsafe_code, reason = "a call into glibc's scalar math (ADR 0007)")]
            pub fn $name($($arg: $ty),+) -> $ret {
                // SAFETY: `ffi::$name` is glibc's function of this C
                // signature. It takes and returns floating-point values
                // only, has no precondition on them, and touches no memory
                // of the caller.
                unsafe { ffi::$name($($arg),+) }
            }
        )+
    };
}

wrappers! {
    extern {
        fn sincosf(x: f32, sin: *mut f32, cos: *mut f32);
        fn lgamma_r(x: f64, sign: *mut core::ffi::c_int) -> f64;
    }
    /// `sin x`, `x` in radians.
    fn sinf(x: f32) -> f32;
    /// `cos x`, `x` in radians.
    fn cosf(x: f32) -> f32;
    /// `tan x`, `x` in radians.
    fn tanf(x: f32) -> f32;
    /// `asin x`, in `[−π/2, π/2]`; NaN outside `[−1, 1]`.
    fn asinf(x: f32) -> f32;
    /// `acos x`, in `[0, π]`; NaN outside `[−1, 1]`.
    fn acosf(x: f32) -> f32;
    /// The angle of `(x, y)`, in `[−π, π]`.
    fn atan2f(y: f32, x: f32) -> f32;
    /// `cosh x`.
    fn coshf(x: f32) -> f32;
    /// `atanh x`; NaN outside `[−1, 1]`.
    fn atanhf(x: f32) -> f32;
    /// `eˣ`.
    fn expf(x: f32) -> f32;
    /// `2ˣ`.
    fn exp2f(x: f32) -> f32;
    /// `ln x`.
    fn logf(x: f32) -> f32;
    /// `xʸ`.
    fn powf(x: f32, y: f32) -> f32;
    /// The error function, `erf x`.
    fn erff(x: f32) -> f32;
    /// `√x`, correctly rounded.
    fn sqrtf(x: f32) -> f32;
    /// `a · b + c` with one rounding.
    fn fmaf(a: f32, b: f32, c: f32) -> f32;
    /// `⌊x⌋`.
    fn floorf(x: f32) -> f32;
    /// `⌈x⌉`.
    fn ceilf(x: f32) -> f32;
    /// `x` rounded to the nearest integer, halfway cases away from zero.
    fn roundf(x: f32) -> f32;
    /// `sin x`, `x` in radians.
    fn sin(x: f64) -> f64;
    /// `cos x`, `x` in radians.
    fn cos(x: f64) -> f64;
    /// `acos x`, in `[0, π]`; NaN outside `[−1, 1]`.
    fn acos(x: f64) -> f64;
    /// The angle of `(x, y)`, in `[−π, π]`.
    fn atan2(y: f64, x: f64) -> f64;
    /// `√(x² + y²)`, without undue overflow or underflow.
    fn hypot(x: f64, y: f64) -> f64;
    /// `eˣ`.
    fn exp(x: f64) -> f64;
    /// `ln x`.
    fn log(x: f64) -> f64;
    /// `xʸ`.
    fn pow(x: f64, y: f64) -> f64;
    /// `∛x`.
    fn cbrt(x: f64) -> f64;
    /// The error function, `erf x`.
    fn erf(x: f64) -> f64;
    /// `√x`, correctly rounded.
    fn sqrt(x: f64) -> f64;
    /// `a · b + c` with one rounding.
    fn fma(a: f64, b: f64, c: f64) -> f64;
}

/// `(sin x, cos x)`, `x` in radians, in one call.
#[inline]
#[must_use]
#[expect(unsafe_code, reason = "a call into glibc's scalar math (ADR 0007)")]
pub fn sincosf(x: f32) -> (f32, f32) {
    let (mut sin, mut cos) = (0.0, 0.0);
    // SAFETY: `ffi::sincosf` is glibc's `sincosf`, which writes one
    // `f32` through each pointer and keeps neither. Both point to distinct,
    // live, aligned locals.
    unsafe { ffi::sincosf(x, &raw mut sin, &raw mut cos) };
    (sin, cos)
}

/// `ln |Γ(x)|`.
///
/// Through glibc's reentrant `lgamma_r`, which returns the sign of `Γ(x)`
/// through a pointer instead of in the global `signgam` that `lgamma`
/// writes; the sign is dropped.
#[inline]
#[must_use]
#[expect(unsafe_code, reason = "a call into glibc's scalar math (ADR 0007)")]
pub fn lgamma(x: f64) -> f64 {
    let mut sign = 0;
    // SAFETY: `ffi::lgamma_r` is glibc's `lgamma_r`, which writes one
    // `int` through the pointer and does not keep it. It points to a live,
    // aligned local.
    unsafe { ffi::lgamma_r(x, &raw mut sign) }
}
