//! The CUDA `libdevice` functions the `nvptx64` backend uses (ADR 0028, as
//! amended by #142), each behind one safe wrapper; nothing else calls the
//! declarations (ADR 0007).
//!
//! `libdevice.10.bc` is NVIDIA's C math for the GPU, in LLVM bitcode. The
//! crate only declares the functions; `umbra-gpu`'s build links the bitcode
//! into the PTX with the toolchain's `llvm-bitcode-linker`, which inlines
//! them, so the PTX keeps no `.extern` (#146). A build that does not link it
//! still compiles, with one `.extern .func __nv_*` per function used.
//!
//! The primitives are one PTX instruction each, with IEEE semantics:
//! [`fma`] is `fma.rn.f32`, [`sqrt`] `sqrt.rn.f32` (not `__nv_sqrtf`, whose
//! rounding depends on the `__CUDA_PREC_SQRT` reflection), [`floor`]
//! `cvt.rmi.f32.f32` and [`ceil`] `cvt.rpi.f32.f32`. `libdevice` picks the
//! `.ftz` variants when `__nvvm_reflect("__CUDA_FTZ")` is 1; LLVM's
//! `NVVMReflect` pass makes it 0 unless the module sets `nvvm-reflect-ftz`,
//! which Umbra's build does not, so subnormals are kept; the GPU tests
//! checks it on subnormal inputs (#146).
//!
//! The transcendentals carry the maximum errors of the CUDA Math API, in
//! ULPs of the correctly rounded result over the whole range, which the GPU
//! tests check against glibc (#126, #146). Internally they use
//! approximate instructions that flush subnormals to zero (`ex2.approx.ftz`,
//! `rcp.approx.ftz`, found by the spike of #142): a result in the subnormal
//! range may come back as zero. The GPU promises statistical equality only
//! (ADR 0026), so ADR 0022's ban on approximate instructions does not apply.

/// The `libdevice` declarations, private to this module: callers use the
/// wrappers below. Rust's `f32` is C's `float` on `nvptx64`.
mod ffi {
    #[expect(
        unsafe_code,
        reason = "FFI to the C math library (ADR 0007): libdevice"
    )]
    unsafe extern "C" {
        pub(super) fn __nv_fmaf(a: f32, b: f32, c: f32) -> f32;
        pub(super) fn __nv_fsqrt_rn(x: f32) -> f32;
        pub(super) fn __nv_floorf(x: f32) -> f32;
        pub(super) fn __nv_ceilf(x: f32) -> f32;
        pub(super) fn __nv_sinf(x: f32) -> f32;
        pub(super) fn __nv_cosf(x: f32) -> f32;
        pub(super) fn __nv_fast_sinf(x: f32) -> f32;
        pub(super) fn __nv_fast_cosf(x: f32) -> f32;
        pub(super) fn __nv_atan2f(y: f32, x: f32) -> f32;
        pub(super) fn __nv_erff(x: f32) -> f32;
        pub(super) fn __nv_expf(x: f32) -> f32;
        pub(super) fn __nv_powf(x: f32, y: f32) -> f32;
        pub(super) fn __nv_roundf(x: f32) -> f32;
        pub(super) fn __nv_tanf(x: f32) -> f32;
        pub(super) fn __nv_asinf(x: f32) -> f32;
        pub(super) fn __nv_acosf(x: f32) -> f32;
        pub(super) fn __nv_logf(x: f32) -> f32;
        pub(super) fn __nv_atanhf(x: f32) -> f32;
        pub(super) fn __nv_coshf(x: f32) -> f32;
    }
}

/// Defines `$name`, with visibility `$vis`, as the safe wrapper of the `libdevice` function `$c`.
macro_rules! wrapper {
    ($(#[$doc:meta])* $vis:vis $name:ident = $c:ident($($arg:ident),+)) => {
        $(#[$doc])*
        #[inline]
        #[must_use]
        #[expect(
            unsafe_code,
            reason = "FFI to the C math library (ADR 0007): libdevice"
        )]
        $vis fn $name($($arg: f32),+) -> f32 {
            // SAFETY: the function is a pure function of its `f32`
            // arguments: it reads and writes no memory, has no precondition
            // and is defined for every input, NaNs and infinities included
            // (C99 semantics, or IEEE for the primitives).
            unsafe { ffi::$c($($arg),+) }
        }
    };
}

wrapper! {
    /// `a · b + c` with one rounding: `fma.rn.f32`.
    pub(crate) fma = __nv_fmaf(a, b, c)
}

wrapper! {
    /// `√x`, correctly rounded: `sqrt.rn.f32`.
    pub(crate) sqrt = __nv_fsqrt_rn(x)
}

wrapper! {
    /// `⌊x⌋`: `cvt.rmi.f32.f32`.
    pub(crate) floor = __nv_floorf(x)
}

wrapper! {
    /// `⌈x⌉`: `cvt.rpi.f32.f32`.
    pub(crate) ceil = __nv_ceilf(x)
}

wrapper! {
    /// `sin(x)`, within 2 ULP, with an exact argument reduction at any
    /// magnitude.
    pub sinf = __nv_sinf(x)
}

wrapper! {
    /// `cos(x)`, within 2 ULP, with an exact argument reduction at any
    /// magnitude.
    pub(crate) cosf = __nv_cosf(x)
}

wrapper! {
    /// `sin(x)` by `sin.approx.f32`: on `[−π, π]` an absolute error of at
    /// most 2⁻²¹·⁴¹, larger outside it.
    pub(crate) fast_sinf = __nv_fast_sinf(x)
}

wrapper! {
    /// `cos(x)` by `cos.approx.f32`: on `[−π, π]` an absolute error of at
    /// most 2⁻²¹·¹⁹, larger outside it.
    pub(crate) fast_cosf = __nv_fast_cosf(x)
}

wrapper! {
    /// `atan2(y, x)`, within 3 ULP.
    pub(crate) atan2f = __nv_atan2f(y, x)
}

wrapper! {
    /// `erf(x)`, within 2 ULP.
    pub(crate) erff = __nv_erff(x)
}

wrapper! {
    /// `eˣ`, within 2 ULP.
    pub expf = __nv_expf(x)
}

wrapper! {
    /// `xʸ`, within 4 ULP.
    pub powf = __nv_powf(x, y)
}

wrapper! {
    /// `x` rounded to the nearest integer, halfway cases away from zero:
    /// exact.
    pub roundf = __nv_roundf(x)
}

wrapper! {
    /// `tan(x)`, within 4 ULP.
    pub tanf = __nv_tanf(x)
}

wrapper! {
    /// `asin(x)`, within 2 ULP.
    pub asinf = __nv_asinf(x)
}

wrapper! {
    /// `acos(x)`, within 2 ULP.
    pub acosf = __nv_acosf(x)
}

wrapper! {
    /// `ln(x)`, within 1 ULP.
    pub logf = __nv_logf(x)
}

wrapper! {
    /// `atanh(x)`, within 3 ULP.
    pub atanhf = __nv_atanhf(x)
}

wrapper! {
    /// `cosh(x)`, within 2 ULP.
    pub coshf = __nv_coshf(x)
}
