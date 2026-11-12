//! The launch parameters of the math program, `__raygen__math`, and the
//! functions it evaluates: the layout shared by the host and the device.
//!
//! As `launch.rs`, `umbra-kernels` and `umbra-gpu` both compile this file,
//! the second by `#[path]`, and only with their `math-program` feature,
//! which the GPU tests turn on (#146). It depends on `core` only.

/// What the host passes to one launch of the math program, in the same
/// `.const` variable `params` as `LaunchParams`, which is no smaller.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub(crate) struct MathParams {
    /// Device address of `count` argument records, `[f32; 3]` each: the
    /// function's arguments in order, the unused ones ignored.
    pub(crate) args: u64,
    /// Device address of `count` `f32` results, written by the launch.
    pub(crate) results: u64,
    /// The number of argument records, at most the launch width.
    pub(crate) count: u32,
    /// The [`MathFunction`], as `u32::from` gives it.
    pub(crate) function: u32,
}

const _: () = {
    use core::mem::offset_of;

    assert!(size_of::<MathParams>() == 24, "`MathParams` is 24 bytes");
    assert!(
        size_of::<MathParams>() <= size_of::<crate::launch::LaunchParams>(),
        "`MathParams` fits the `params` variable, sized for `LaunchParams`"
    );
    assert!(offset_of!(MathParams, args) == 0, "`args` is at byte 0");
    assert!(
        offset_of!(MathParams, results) == 8,
        "`results` is at byte 8"
    );
    assert!(offset_of!(MathParams, count) == 16, "`count` is at byte 16");
    assert!(
        offset_of!(MathParams, function) == 20,
        "`function` is at byte 20"
    );
};

/// Defines [`MathFunction`] with the variants listed, in order, and
/// `MathFunction::ALL`, the same list, so that a variant's id is its index.
macro_rules! math_functions {
    ($($(#[$doc:meta])+ $name:ident,)+) => {
        /// A function of `umbra-math` that the math program evaluates on the
        /// GPU, where it is the `nvptx64` backend's: CUDA's `libdevice`, or
        /// Umbra's own kernel built on it. Its id in `MathParams` is
        /// `u32::from(function)`.
        #[derive(Debug, Clone, Copy, PartialEq, Eq)]
        #[repr(u32)]
        pub enum MathFunction {
            $($(#[$doc])+ $name,)+
        }

        impl MathFunction {
            /// Every function, in order: a function's id is its index.
            const ALL: &[Self] = &[$(Self::$name),+];
        }
    };
}

math_functions! {
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
    /// `__nv_atan2f`, with subnormal arguments scaled.
    Atan2,
    /// `umbra_math::erf(x)`: `__nv_erff`.
    Erf,
    /// `umbra_math::expf(x)`: `__nv_expf`.
    Expf,
    /// `umbra_math::powf(x, y)`: `__nv_powf`, with subnormal arguments
    /// scaled.
    Powf,
    /// `umbra_math::logf(x)`: `__nv_logf`.
    Logf,
    /// `umbra_math::atanhf(x)`: `__nv_atanhf`.
    Atanhf,
    /// `umbra_math::coshf(x)`: `__nv_coshf`.
    Coshf,
}

impl From<MathFunction> for u32 {
    /// The function's id: its discriminant.
    fn from(function: MathFunction) -> Self {
        function as Self
    }
}

/// An id that names no [`MathFunction`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct UnknownMathFunction;

impl TryFrom<u32> for MathFunction {
    type Error = UnknownMathFunction;

    /// The function whose id is `id`.
    fn try_from(id: u32) -> Result<Self, Self::Error> {
        usize::try_from(id)
            .ok()
            .and_then(|index| Self::ALL.get(index))
            .copied()
            .ok_or(UnknownMathFunction)
    }
}
