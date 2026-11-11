//! Floating-point constants, bit manipulation and the conservative rounding
//! operations behind `Interval` (pbrt-v4 `src/pbrt/util/float.h`).
//!
//! PBR 4e §6.8.1–§6.8.2. On the CPU, pbrt-v4 rounds up or down by stepping one
//! ULP with `NextFloatUp`/`NextFloatDown` after the round-to-nearest result;
//! Umbra does the same and never changes the rounding mode (ADR 0001).
//!
//! [`fma`], [`sqrt`], [`floor`] and [`ceil`] are the hardware instruction,
//! with the bits of glibc's routines; the wrappers of those
//! ([`fmaf`](crate::fmaf), [`sqrtf`](crate::sqrtf), [`floorf`](crate::floorf),
//! [`ceilf`](crate::ceilf)) are the test oracle in
//! `tests/umbra_math/reference/` (ADR 0007). [`next_float_up`] and
//! [`next_float_down`] keep pbrt-v4's branches: a branch-free form measured
//! slower, since the branch on infinity is perfectly predicted, and the
//! `Interval` kernels step four lanes at once instead.

mod arch;

pub(crate) use arch::fma_f64;

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
/// Umbra uses it where pbrt-v4 uses `FMA`, and anywhere else it is faster or
/// more accurate, except in code whose error bounds pbrt-v4 derives from its
/// exact operations (ADR 0022). It is the hardware instruction
/// (`vfmadd213ss`), which gives the bits of the correctly rounded
/// [`fmaf`](crate::fmaf).
///
/// pbrt-v4 `FMA`.
#[inline]
#[must_use]
pub fn fma(a: f32, b: f32, c: f32) -> f32 {
    arch::fma(a, b, c)
}

/// `√x`, correctly rounded: the hardware square root, with the bits of
/// [`sqrtf`](crate::sqrtf).
///
/// pbrt-v4 `std::sqrt`.
#[inline]
#[must_use]
pub fn sqrt(x: f32) -> f32 {
    arch::sqrt(x)
}

/// `⌊x⌋`: the hardware rounding instruction (SSE4.1 `roundss`). Exact, so
/// the bits of [`floorf`](crate::floorf).
///
/// pbrt-v4 `std::floor`.
#[inline]
#[must_use]
pub fn floor(x: f32) -> f32 {
    arch::floor(x)
}

/// `⌈x⌉`: the hardware rounding instruction (SSE4.1 `roundss`). Exact, so
/// the bits of [`ceilf`](crate::ceilf).
///
/// pbrt-v4 `std::ceil`.
#[inline]
#[must_use]
pub fn ceil(x: f32) -> f32 {
    arch::ceil(x)
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
#[inline]
#[must_use]
pub fn next_float_up(v: f32) -> f32 {
    if v.is_infinite() && v > 0.0 {
        return v;
    }
    let v = if v == 0.0 { 0.0 } else { v };
    let bits = v.to_bits();
    // Wrapping, as pbrt-v4's unsigned arithmetic, so NaN bits never overflow.
    f32::from_bits(if v >= 0.0 {
        bits.wrapping_add(1)
    } else {
        bits.wrapping_sub(1)
    })
}

/// The next `f32` below `v`. `−∞` stays `−∞`, and `+0` steps to the largest
/// negative subnormal.
///
/// PBR 4e §6.8.1, pbrt-v4 `NextFloatDown`.
#[inline]
#[must_use]
pub fn next_float_down(v: f32) -> f32 {
    if v.is_infinite() && v < 0.0 {
        return v;
    }
    let v = if v == 0.0 { -0.0 } else { v };
    let bits = v.to_bits();
    // Wrapping, as pbrt-v4's unsigned arithmetic, so NaN bits never overflow.
    f32::from_bits(if v > 0.0 {
        bits.wrapping_sub(1)
    } else {
        bits.wrapping_add(1)
    })
}

/// `a + b`, rounded so the result is not below the exact sum.
///
/// PBR 4e §6.8.2, pbrt-v4 `AddRoundUp`.
#[inline]
#[must_use]
pub fn add_round_up(a: f32, b: f32) -> f32 {
    next_float_up(a + b)
}

/// `a + b`, rounded so the result is not above the exact sum.
///
/// PBR 4e §6.8.2, pbrt-v4 `AddRoundDown`.
#[inline]
#[must_use]
pub fn add_round_down(a: f32, b: f32) -> f32 {
    next_float_down(a + b)
}

/// `a − b`, rounded up.
///
/// PBR 4e §6.8.2, pbrt-v4 `SubRoundUp`.
#[inline]
#[must_use]
pub fn sub_round_up(a: f32, b: f32) -> f32 {
    add_round_up(a, -b)
}

/// `a − b`, rounded down.
///
/// PBR 4e §6.8.2, pbrt-v4 `SubRoundDown`.
#[inline]
#[must_use]
pub fn sub_round_down(a: f32, b: f32) -> f32 {
    add_round_down(a, -b)
}

/// `a · b`, rounded up.
///
/// PBR 4e §6.8.2, pbrt-v4 `MulRoundUp`.
#[inline]
#[must_use]
pub fn mul_round_up(a: f32, b: f32) -> f32 {
    next_float_up(a * b)
}

/// `a · b`, rounded down.
///
/// PBR 4e §6.8.2, pbrt-v4 `MulRoundDown`.
#[inline]
#[must_use]
pub fn mul_round_down(a: f32, b: f32) -> f32 {
    next_float_down(a * b)
}

/// `a / b`, rounded up.
///
/// PBR 4e §6.8.2, pbrt-v4 `DivRoundUp`.
#[inline]
#[must_use]
pub fn div_round_up(a: f32, b: f32) -> f32 {
    next_float_up(a / b)
}

/// `a / b`, rounded down.
///
/// PBR 4e §6.8.2, pbrt-v4 `DivRoundDown`.
#[inline]
#[must_use]
pub fn div_round_down(a: f32, b: f32) -> f32 {
    next_float_down(a / b)
}

/// `√a`, rounded up.
///
/// PBR 4e §6.8.2, pbrt-v4 `SqrtRoundUp`.
#[inline]
#[must_use]
pub fn sqrt_round_up(a: f32) -> f32 {
    next_float_up(sqrt(a))
}

/// `√a`, rounded down and never below zero.
///
/// PBR 4e §6.8.2, pbrt-v4 `SqrtRoundDown`.
#[inline]
#[must_use]
pub fn sqrt_round_down(a: f32) -> f32 {
    crate::max(0.0, next_float_down(sqrt(a)))
}

/// `a · b + c` with one rounding, rounded up.
///
/// PBR 4e §6.8.2, pbrt-v4 `FMARoundUp`.
#[inline]
#[must_use]
pub fn fma_round_up(a: f32, b: f32, c: f32) -> f32 {
    next_float_up(fma(a, b, c))
}

/// `a · b + c` with one rounding, rounded down.
///
/// PBR 4e §6.8.2, pbrt-v4 `FMARoundDown`.
#[inline]
#[must_use]
pub fn fma_round_down(a: f32, b: f32, c: f32) -> f32 {
    next_float_down(fma(a, b, c))
}
