//! Vectors, points, normals, frames, transforms, `Interval` and floating-point error bounds, `libm` wrappers.
//!
//! A core crate (ADR 0014, ADR 0015): `#![no_std]` without `alloc`. Everything
//! here is `f32` (ADR 0001) and follows the portable bit-identical rules of
//! ADR 0002: transcendentals come from the `libm` crate, FMA is explicit
//! ([`fma`]), and the [`Vec4`] SIMD facade computes exactly what its scalar
//! reference computes. Names follow pbrt-v4, in snake case.

#![no_std]

mod float;
mod interval;
mod math;
mod simd;
mod square_matrix;
mod transform;
mod vecmath;

pub use float::{
    MACHINE_EPSILON, ONE_MINUS_EPSILON, add_round_down, add_round_up, div_round_down, div_round_up,
    exponent, fma, fma_round_down, fma_round_up, gamma, mul_round_down, mul_round_up,
    next_float_down, next_float_up, sign_bit, significand, sqrt_round_down, sqrt_round_up,
    sub_round_down, sub_round_up,
};
pub use interval::Interval;
pub use math::{
    CompensatedFloat, CompensatedSum, INV_2_PI, INV_4_PI, INV_PI, Log2Int, PI, PI_OVER_2,
    PI_OVER_4, PowerOf2, Products, Real, SQRT_2, clamp, degrees, difference_of_products, erf_inv,
    evaluate_polynomial, fast_exp, find_interval, gaussian, gaussian_integral, inner_product,
    is_power_of_2, lerp, log2_int, max, min, quadratic, radians, round_up_pow2, safe_acos,
    safe_asin, safe_sqrt, sin_x_over_x, smooth_step, sqr, sum_of_products, two_prod, two_sum,
};
pub use simd::{Mask4, Vec4};
pub use square_matrix::SquareMatrix;
pub use transform::{
    Transform, Transformable, look_at, orthographic, perspective, rotate, rotate_from_to,
    rotate_sin_cos, rotate_x, rotate_y, rotate_z, scale, translate,
};
pub use vecmath::*;
