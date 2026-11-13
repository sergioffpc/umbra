//! The math program, `__raygen__math`: one function of `umbra-math`
//! evaluated per thread, on arguments the host wrote, for the GPU tests that
//! check the GPU's math against the CPU's (#146). Built only with the
//! `math-program` feature, which the GPU tests turn on through `umbra-gpu`.

use core::mem::offset_of;

use crate::math_launch::{MathFunction, MathParams};
use crate::optix;

impl MathFunction {
    /// The function of `umbra-math` on its arguments, in order: `a`, then
    /// `b`, then `c`, as far as it takes any.
    pub(crate) fn evaluate(self, [a, b, c]: [f32; 3]) -> f32 {
        match self {
            Self::Fma => umbra_math::fma(a, b, c),
            Self::Sqrt => umbra_math::sqrt(a),
            Self::Floor => umbra_math::floor(a),
            Self::Ceil => umbra_math::ceil(a),
            Self::Roundf => umbra_math::roundf(a),
            Self::Sinf => umbra_math::sinf(a),
            Self::SinOfSinCos => umbra_math::sin_cos(a).0,
            Self::CosOfSinCos => umbra_math::sin_cos(a).1,
            Self::Tanf => umbra_math::tanf(a),
            Self::Asinf => umbra_math::asinf(a),
            Self::Acosf => umbra_math::acosf(a),
            Self::Atan2 => umbra_math::atan2(a, b),
            Self::Erf => umbra_math::erf(a),
            Self::Expf => umbra_math::expf(a),
            Self::Powf => umbra_math::powf(a, b),
            Self::Logf => umbra_math::logf(a),
            Self::Atanhf => umbra_math::atanhf(a),
            Self::Coshf => umbra_math::coshf(a),
        }
    }
}

/// The launch parameters of a math launch, field by field from the `.const`
/// variable.
pub(crate) fn math_params() -> MathParams {
    MathParams {
        args: optix::launch_param_u64::<{ offset_of!(MathParams, args) }>(),
        results: optix::launch_param_u64::<{ offset_of!(MathParams, results) }>(),
        count: optix::launch_param_u32::<{ offset_of!(MathParams, count) }>(),
        function: optix::launch_param_u32::<{ offset_of!(MathParams, function) }>(),
    }
}

/// Argument record `i` of the array at device address `args`.
#[expect(
    unsafe_code,
    reason = "reads a buffer the host wrote, by its device address (ADR 0007, ADR 0028)"
)]
pub(crate) fn read_args(args: u64, i: u32) -> [f32; 3] {
    let record = (args as *const [f32; 3]).wrapping_add(i as usize);
    // SAFETY: the host points `args` at `count` records, aligned and
    // initialised, and the caller checked `i < count`.
    unsafe { record.read() }
}

/// Writes result `i` of the array at device address `results`.
#[expect(
    unsafe_code,
    reason = "writes a buffer the host reads, by its device address (ADR 0007, ADR 0028)"
)]
pub(crate) fn write_result(results: u64, i: u32, result: f32) {
    let slot = (results as *mut f32).wrapping_add(i as usize);
    // SAFETY: the host points `results` at `count` aligned `f32`s that only
    // thread `i` writes, and the caller checked `i < count`.
    unsafe { slot.write(result) }
}
