//! The reference of `umbra_math`'s `float` module: pbrt-v4's
//! `NextFloatUp`/`NextFloatDown` with their branches, and glibc's routines
//! for `fma`, `sqrt`, `floor` and `ceil`.

use umbra_math::cmath;

/// pbrt-v4 `FMA`, through glibc's `fmaf`.
#[must_use]
pub(crate) fn fma(a: f32, b: f32, c: f32) -> f32 {
    cmath::fmaf(a, b, c)
}

/// `√x`, through glibc's `sqrtf`.
#[must_use]
pub(crate) fn sqrt(x: f32) -> f32 {
    cmath::sqrtf(x)
}

/// `⌊x⌋`, through glibc's `floorf`.
#[must_use]
pub(crate) fn floor(x: f32) -> f32 {
    cmath::floorf(x)
}

/// `⌈x⌉`, through glibc's `ceilf`.
#[must_use]
pub(crate) fn ceil(x: f32) -> f32 {
    cmath::ceilf(x)
}

/// pbrt-v4 `NextFloatUp`, as written: early return for `+∞`, `-0` mapped to
/// `+0`, then one step up or down by the sign.
#[must_use]
pub(crate) fn next_float_up(v: f32) -> f32 {
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

/// pbrt-v4 `NextFloatDown`, as written.
#[must_use]
pub(crate) fn next_float_down(v: f32) -> f32 {
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

/// pbrt-v4 `AddRoundUp`.
#[must_use]
pub(crate) fn add_round_up(a: f32, b: f32) -> f32 {
    next_float_up(a + b)
}

/// pbrt-v4 `AddRoundDown`.
#[must_use]
pub(crate) fn add_round_down(a: f32, b: f32) -> f32 {
    next_float_down(a + b)
}

/// pbrt-v4 `SubRoundUp`.
#[must_use]
pub(crate) fn sub_round_up(a: f32, b: f32) -> f32 {
    add_round_up(a, -b)
}

/// pbrt-v4 `SubRoundDown`.
#[must_use]
pub(crate) fn sub_round_down(a: f32, b: f32) -> f32 {
    add_round_down(a, -b)
}

/// pbrt-v4 `MulRoundUp`.
#[must_use]
pub(crate) fn mul_round_up(a: f32, b: f32) -> f32 {
    next_float_up(a * b)
}

/// pbrt-v4 `MulRoundDown`.
#[must_use]
pub(crate) fn mul_round_down(a: f32, b: f32) -> f32 {
    next_float_down(a * b)
}

/// pbrt-v4 `DivRoundUp`.
#[must_use]
pub(crate) fn div_round_up(a: f32, b: f32) -> f32 {
    next_float_up(a / b)
}

/// pbrt-v4 `DivRoundDown`.
#[must_use]
pub(crate) fn div_round_down(a: f32, b: f32) -> f32 {
    next_float_down(a / b)
}

/// pbrt-v4 `SqrtRoundUp`.
#[must_use]
pub(crate) fn sqrt_round_up(a: f32) -> f32 {
    next_float_up(sqrt(a))
}

/// pbrt-v4 `SqrtRoundDown`.
#[must_use]
pub(crate) fn sqrt_round_down(a: f32) -> f32 {
    umbra_math::max(0.0, next_float_down(sqrt(a)))
}

/// pbrt-v4 `FMARoundUp`.
#[must_use]
pub(crate) fn fma_round_up(a: f32, b: f32, c: f32) -> f32 {
    next_float_up(fma(a, b, c))
}

/// pbrt-v4 `FMARoundDown`.
#[must_use]
pub(crate) fn fma_round_down(a: f32, b: f32, c: f32) -> f32 {
    next_float_down(fma(a, b, c))
}
