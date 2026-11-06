//! Floating-point constants, bit manipulation and the conservative rounding
//! operations behind `Interval` (pbrt-v4 `src/pbrt/util/float.h`).
//!
//! PBR 4e §6.8.1–§6.8.2. On the CPU, pbrt-v4 rounds up or down by stepping one
//! ULP with `NextFloatUp`/`NextFloatDown` after the round-to-nearest result;
//! Umbra does the same and never changes the rounding mode (ADR 0002).

/// Half the distance between 1 and the next `f32`: the bound on the relative
/// error of one correctly rounded operation.
///
/// PBR 4e §6.8.1, pbrt-v4 `MachineEpsilon`.
pub const MACHINE_EPSILON: f32 = f32::EPSILON * 0.5;

/// The largest `f32` below 1, so `[0, 1)` samples never reach 1.
///
/// PBR 4e §2.1, pbrt-v4 `OneMinusEpsilon`.
pub const ONE_MINUS_EPSILON: f32 = f32::from_bits(0x3f7f_ffff);

/// The bound `γ(n) = nε / (1 − nε)` on the relative error of `n` chained
/// floating-point operations.
///
/// PBR 4e §6.8.1, pbrt-v4 `gamma`.
#[must_use]
pub const fn gamma(n: u16) -> f32 {
    // `f32::from` is not const; u16 to f32 is exact.
    let n = n as f32;
    (n * MACHINE_EPSILON) / (1.0 - n * MACHINE_EPSILON)
}

/// The fused multiply-add `a * b + c` with a single rounding.
///
/// Umbra uses it exactly where pbrt-v4 uses `FMA` and nowhere else (ADR 0002).
/// `libm::fmaf` compiles to the hardware instruction where the build target
/// has one, and is a correctly rounded software routine otherwise.
///
/// pbrt-v4 `FMA`.
#[inline]
#[must_use]
pub fn fma(a: f32, b: f32, c: f32) -> f32 {
    libm::fmaf(a, b, c)
}

/// The unbiased exponent of `v`.
///
/// PBR 4e §6.8.1, pbrt-v4 `Exponent`.
#[must_use]
pub const fn exponent(v: f32) -> i32 {
    #[expect(clippy::cast_possible_wrap, reason = "an 8-bit field")]
    let biased = (v.to_bits() >> 23) as i32;
    biased - 127
}

/// The 23 significand bits of `v`, without the implicit leading one.
///
/// PBR 4e §6.8.1, pbrt-v4 `Significand`.
#[must_use]
pub const fn significand(v: f32) -> u32 {
    v.to_bits() & ((1 << 23) - 1)
}

/// The sign bit of `v`, in place.
///
/// PBR 4e §6.8.1, pbrt-v4 `SignBit`.
#[must_use]
pub const fn sign_bit(v: f32) -> u32 {
    v.to_bits() & 0x8000_0000
}

/// The next `f32` above `v`. `+∞` stays `+∞`, and `-0` steps to the smallest
/// positive subnormal.
///
/// PBR 4e §6.8.1, pbrt-v4 `NextFloatUp`.
#[must_use]
pub fn next_float_up(v: f32) -> f32 {
    if v.is_infinite() && v > 0.0 {
        return v;
    }
    let v = if v == 0.0 { 0.0 } else { v };
    let bits = v.to_bits();
    f32::from_bits(if v >= 0.0 { bits + 1 } else { bits - 1 })
}

/// The next `f32` below `v`. `−∞` stays `−∞`, and `+0` steps to the largest
/// negative subnormal.
///
/// PBR 4e §6.8.1, pbrt-v4 `NextFloatDown`.
#[must_use]
pub fn next_float_down(v: f32) -> f32 {
    if v.is_infinite() && v < 0.0 {
        return v;
    }
    let v = if v == 0.0 { -0.0 } else { v };
    let bits = v.to_bits();
    f32::from_bits(if v > 0.0 { bits - 1 } else { bits + 1 })
}

/// `a + b`, rounded so the result is not below the exact sum.
///
/// PBR 4e §6.8.2, pbrt-v4 `AddRoundUp`.
#[must_use]
pub fn add_round_up(a: f32, b: f32) -> f32 {
    next_float_up(a + b)
}

/// `a + b`, rounded so the result is not above the exact sum.
///
/// PBR 4e §6.8.2, pbrt-v4 `AddRoundDown`.
#[must_use]
pub fn add_round_down(a: f32, b: f32) -> f32 {
    next_float_down(a + b)
}

/// `a − b`, rounded up.
///
/// PBR 4e §6.8.2, pbrt-v4 `SubRoundUp`.
#[must_use]
pub fn sub_round_up(a: f32, b: f32) -> f32 {
    add_round_up(a, -b)
}

/// `a − b`, rounded down.
///
/// PBR 4e §6.8.2, pbrt-v4 `SubRoundDown`.
#[must_use]
pub fn sub_round_down(a: f32, b: f32) -> f32 {
    add_round_down(a, -b)
}

/// `a · b`, rounded up.
///
/// PBR 4e §6.8.2, pbrt-v4 `MulRoundUp`.
#[must_use]
pub fn mul_round_up(a: f32, b: f32) -> f32 {
    next_float_up(a * b)
}

/// `a · b`, rounded down.
///
/// PBR 4e §6.8.2, pbrt-v4 `MulRoundDown`.
#[must_use]
pub fn mul_round_down(a: f32, b: f32) -> f32 {
    next_float_down(a * b)
}

/// `a / b`, rounded up.
///
/// PBR 4e §6.8.2, pbrt-v4 `DivRoundUp`.
#[must_use]
pub fn div_round_up(a: f32, b: f32) -> f32 {
    next_float_up(a / b)
}

/// `a / b`, rounded down.
///
/// PBR 4e §6.8.2, pbrt-v4 `DivRoundDown`.
#[must_use]
pub fn div_round_down(a: f32, b: f32) -> f32 {
    next_float_down(a / b)
}

/// `√a`, rounded up.
///
/// PBR 4e §6.8.2, pbrt-v4 `SqrtRoundUp`.
#[must_use]
pub fn sqrt_round_up(a: f32) -> f32 {
    next_float_up(libm::sqrtf(a))
}

/// `√a`, rounded down and never below zero.
///
/// PBR 4e §6.8.2, pbrt-v4 `SqrtRoundDown`.
#[must_use]
pub fn sqrt_round_down(a: f32) -> f32 {
    crate::max(0.0, next_float_down(libm::sqrtf(a)))
}

/// `a · b + c` with one rounding, rounded up.
///
/// PBR 4e §6.8.2, pbrt-v4 `FMARoundUp`.
#[must_use]
pub fn fma_round_up(a: f32, b: f32, c: f32) -> f32 {
    next_float_up(fma(a, b, c))
}

/// `a · b + c` with one rounding, rounded down.
///
/// PBR 4e §6.8.2, pbrt-v4 `FMARoundDown`.
#[must_use]
pub fn fma_round_down(a: f32, b: f32, c: f32) -> f32 {
    next_float_down(fma(a, b, c))
}
