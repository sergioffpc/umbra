#![doc = include_str!("../README.md")]
#![no_std]
// No `f64` in the core (ADR 0028): `clippy.toml` lists it.
#![warn(clippy::disallowed_types)]

// The x86-64 CPU baseline (ADR 0023): every target machine has FMA, and the
// kernels use it, SSE4.1 and AVX2 unconditionally. Without them the build
// stops here rather than run a slower, differently compiled path. Rustdoc
// generates no code and does not see RUSTFLAGS, so `cargo doc` is exempt.
#[cfg(all(
    target_arch = "x86_64",
    not(doc),
    not(all(
        target_feature = "fma",
        target_feature = "sse4.1",
        target_feature = "avx2"
    ))
))]
compile_error!(concat!(
    "umbra-math needs x86-64-v3 (FMA, SSE4.1, AVX2) on x86-64 (ADR 0023): ",
    "build with `-C target-cpu=x86-64-v3`, or `target-cpu=native` on a CPU that has it"
));

// The three supported architectures: two CPUs, x86-64 with `x86-64-v3` and
// arm64 (ADR 0032), and the GPUs, which run the core as `nvptx64` (ADR
// 0028). The SIMD facade, the scalar primitives and the transcendentals
// have a backend for each and no fallback.
#[cfg(not(any(
    target_arch = "x86_64",
    target_arch = "aarch64",
    target_arch = "nvptx64"
)))]
compile_error!(
    "umbra-math supports x86-64 (x86-64-v3), arm64 and nvptx64 only (ADR 0032, ADR 0028)"
);

// The one supported operating system: the CPUs run Linux, and both CPU
// backends call glibc's `libm.so.6`. macOS, Windows and musl are not
// supported (ADR 0034).
#[cfg(all(
    any(target_arch = "x86_64", target_arch = "aarch64"),
    not(all(target_os = "linux", target_env = "gnu"))
))]
compile_error!("umbra-math supports Linux with glibc only");

#[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
pub mod double;
mod float;
mod interval;
#[cfg(target_arch = "nvptx64")]
mod libdevice;
mod math;
mod platform;
mod simd;
mod square_matrix;
mod transcendental;
mod transform;
mod vecmath;

pub use float::{
    MACHINE_EPSILON, ONE_MINUS_EPSILON, add_round_down, add_round_up, ceil, div_round_down,
    div_round_up, exponent, floor, fma, fma_round_down, fma_round_up, gamma, mul_round_down,
    mul_round_up, next_float_down, next_float_up, sign_bit, significand, sqrt, sqrt_round_down,
    sqrt_round_up, sub_round_down, sub_round_up,
};
pub use interval::Interval;
pub use math::{
    CompensatedFloat, CompensatedSum, INV_2_PI, INV_4_PI, INV_PI, Log2Int, PI, PI_OVER_2,
    PI_OVER_4, PowerOf2, Products, Real, SQRT_2, clamp, degrees, difference_of_products, erf_inv,
    evaluate_polynomial, fast_exp, find_interval, gaussian, gaussian_integral, inner_product,
    is_power_of_2, lerp, log2_int, max, min, quadratic, radians, round_up_pow2, safe_acos,
    safe_asin, safe_sqrt, sin_x_over_x, smooth_step, sqr, sum_of_products, two_prod, two_sum,
};
#[cfg(any(target_arch = "x86_64", target_arch = "aarch64"))]
pub use platform::{
    acosf, asinf, atan2f, atanhf, ceilf, cosf, coshf, erff, exp2f, expf, floorf, fmaf, logf, powf,
    roundf, sincosf, sinf, sqrtf, tanf,
};
#[cfg(target_arch = "nvptx64")]
pub use platform::{acosf, asinf, atanhf, coshf, expf, logf, powf, roundf, sinf, tanf};
pub use simd::{Mask4, Vec4};
pub use square_matrix::SquareMatrix;
pub use transcendental::{atan2, erf, sin_cos};
pub use transform::{
    Transform, Transformable, look_at, orthographic, perspective, rotate, rotate_from_to,
    rotate_sin_cos, rotate_x, rotate_y, rotate_z, scale, translate,
};
pub use vecmath::*;
