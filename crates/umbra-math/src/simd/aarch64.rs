//! The NEON backend of [`Vec4`](crate::Vec4) (ADR 0007): each operation is
//! one `core::arch` intrinsic on a `float32x4_t`, chosen so that every lane
//! computes what the scalar operation computes on that lane:
//!
//! - `min_std`/`max_std` are a compare and a `bsl` select, not `fmin`/`fmax`
//!   (which return the NaN operand) nor `fminnm`/`fmaxnm` (which return the
//!   number), neither of which is C++ `std::min`/`std::max`.
//! - `mul_add` is `fmla`, always fused.
//!
//! `float32x4_t` and `[f32; 4]` have the same size, and every bit pattern is
//! a valid value of both, so the two are converted by `transmute`, which
//! compiles to nothing. This module is compiled only when `neon` is enabled
//! for the whole build (`cfg(target_feature = "neon")`), so every CPU the
//! binary runs on has its instructions.

use core::arch::aarch64::{
    float32x2_t, float32x4_t, int32x4_t, uint32x4_t, vabsq_f32, vaddq_f32, vaddq_s32, vaddvq_u32,
    vandq_u32, vbslq_f32, vceqq_f32, vcgeq_f32, vcgtq_f32, vcleq_f32, vcltq_f32, vcombine_f32,
    vdivq_f32, veorq_u32, vfmaq_f32, vget_high_f32, vget_low_f32, vmulq_f32, vmvnq_u32, vnegq_f32,
    vorrq_u32, vrev64q_f32, vsqrtq_f32, vsubq_f32, vuzp1q_f32, vuzp2q_f32, vzip1q_f32, vzip2q_f32,
};

use super::{F4, M4};

/// The lanes as a register.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): see the module docs"
)]
fn f(a: F4) -> float32x4_t {
    // SAFETY: `[f32; 4]` and `float32x4_t` are both 16 bytes of four `f32`
    // lanes, and every bit pattern is a valid value of both.
    unsafe { core::mem::transmute::<F4, float32x4_t>(a) }
}

/// The register as lanes.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): see the module docs"
)]
fn a(v: float32x4_t) -> F4 {
    // SAFETY: `float32x4_t` and `[f32; 4]` are both 16 bytes of four `f32`
    // lanes, and every bit pattern is a valid value of both.
    unsafe { core::mem::transmute::<float32x4_t, F4>(v) }
}

/// The mask lanes as a register.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): see the module docs"
)]
fn m(a: M4) -> uint32x4_t {
    // SAFETY: `[u32; 4]` and `uint32x4_t` are both 16 bytes of four 32-bit
    // lanes, and every bit pattern is a valid value of both.
    unsafe { core::mem::transmute::<M4, uint32x4_t>(a) }
}

/// The register as mask lanes.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): see the module docs"
)]
fn u(v: uint32x4_t) -> M4 {
    // SAFETY: `uint32x4_t` and `[u32; 4]` are both 16 bytes of four 32-bit
    // lanes, and every bit pattern is a valid value of both.
    unsafe { core::mem::transmute::<uint32x4_t, M4>(v) }
}

/// The lanes as a signed integer register.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): see the module docs"
)]
fn i(a: F4) -> int32x4_t {
    // SAFETY: `[f32; 4]` and `int32x4_t` are both 16 bytes of four 32-bit
    // lanes, and every bit pattern is a valid value of both.
    unsafe { core::mem::transmute::<F4, int32x4_t>(a) }
}

/// The signed integer register as lanes.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): see the module docs"
)]
fn ai(v: int32x4_t) -> F4 {
    // SAFETY: `int32x4_t` and `[f32; 4]` are both 16 bytes of four 32-bit
    // lanes, and every bit pattern is a valid value of both.
    unsafe { core::mem::transmute::<int32x4_t, F4>(v) }
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn add(x: F4, y: F4) -> F4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    a(unsafe { vaddq_f32(f(x), f(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn sub(x: F4, y: F4) -> F4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    a(unsafe { vsubq_f32(f(x), f(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn mul(x: F4, y: F4) -> F4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    a(unsafe { vmulq_f32(f(x), f(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn div(x: F4, y: F4) -> F4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    a(unsafe { vdivq_f32(f(x), f(y)) })
}

/// `fneg`: the sign bit flipped.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn neg(x: F4) -> F4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    a(unsafe { vnegq_f32(f(x)) })
}

/// `fabs`: the sign bit cleared.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn abs(x: F4) -> F4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    a(unsafe { vabsq_f32(f(x)) })
}

/// `fmla`: `z + x · y` with one rounding.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn mul_add(x: F4, y: F4, z: F4) -> F4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    a(unsafe { vfmaq_f32(f(z), f(x), f(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn sqrt(x: F4) -> F4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    a(unsafe { vsqrtq_f32(f(x)) })
}

/// `y` where `y < x`, else `x`: `std::min(x, y)`.
#[inline]
pub(crate) fn min_std(x: F4, y: F4) -> F4 {
    select(cmp_lt(y, x), y, x)
}

/// `y` where `x < y`, else `x`: `std::max(x, y)`.
#[inline]
pub(crate) fn max_std(x: F4, y: F4) -> F4 {
    select(cmp_lt(x, y), y, x)
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn cmp_eq(x: F4, y: F4) -> M4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    u(unsafe { vceqq_f32(f(x), f(y)) })
}

#[inline]
pub(crate) fn cmp_ne(x: F4, y: F4) -> M4 {
    mask_not(cmp_eq(x, y))
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn cmp_lt(x: F4, y: F4) -> M4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    u(unsafe { vcltq_f32(f(x), f(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn cmp_le(x: F4, y: F4) -> M4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    u(unsafe { vcleq_f32(f(x), f(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn cmp_gt(x: F4, y: F4) -> M4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    u(unsafe { vcgtq_f32(f(x), f(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn cmp_ge(x: F4, y: F4) -> M4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    u(unsafe { vcgeq_f32(f(x), f(y)) })
}

/// `bsl`: `t` where the mask is set, `fl` elsewhere.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn select(mask: M4, t: F4, fl: F4) -> F4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    a(unsafe { vbslq_f32(m(mask), f(t), f(fl)) })
}

/// The lanes as `i32`, added with wrapping.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn add_i32(x: F4, y: F4) -> F4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    ai(unsafe { vaddq_s32(i(x), i(y)) })
}

/// `zip1`: `[a0, b0, a1, b1]`.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn unpack_lo(x: F4, y: F4) -> F4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    a(unsafe { vzip1q_f32(f(x), f(y)) })
}

/// `zip2`: `[a2, b2, a3, b3]`.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn unpack_hi(x: F4, y: F4) -> F4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    a(unsafe { vzip2q_f32(f(x), f(y)) })
}

/// The low two lanes.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
fn low(x: float32x4_t) -> float32x2_t {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    unsafe { vget_low_f32(x) }
}

/// The high two lanes.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
fn high(x: float32x4_t) -> float32x2_t {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    unsafe { vget_high_f32(x) }
}

/// The two halves as one register.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
fn combine(lo: float32x2_t, hi: float32x2_t) -> float32x4_t {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    unsafe { vcombine_f32(lo, hi) }
}

/// `[a0, a1, b0, b1]`.
#[inline]
pub(crate) fn move_lh(x: F4, y: F4) -> F4 {
    a(combine(low(f(x)), low(f(y))))
}

/// `[b2, b3, a2, a3]`.
#[inline]
pub(crate) fn move_hl(x: F4, y: F4) -> F4 {
    a(combine(high(f(y)), high(f(x))))
}

/// `uzp1`: the even lanes, `[a0, a2, b0, b2]`.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn unzip_lo(x: F4, y: F4) -> F4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    a(unsafe { vuzp1q_f32(f(x), f(y)) })
}

/// `uzp2`: the odd lanes, `[a1, a3, b1, b3]`.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn unzip_hi(x: F4, y: F4) -> F4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    a(unsafe { vuzp2q_f32(f(x), f(y)) })
}

/// `rev64`: `[a1, a0, a3, a2]`.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn swap_pairs(x: F4) -> F4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    a(unsafe { vrev64q_f32(f(x)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn mask_not(x: M4) -> M4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    u(unsafe { vmvnq_u32(m(x)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn mask_and(x: M4, y: M4) -> M4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    u(unsafe { vandq_u32(m(x), m(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn mask_or(x: M4, y: M4) -> M4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    u(unsafe { vorrq_u32(m(x), m(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn mask_xor(x: M4, y: M4) -> M4 {
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    u(unsafe { veorq_u32(m(x), m(y)) })
}

/// Bit `i` set for each lane `i` that is set: the lanes masked to
/// `[1, 2, 4, 8]` and summed.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one NEON intrinsic, see the module docs"
)]
pub(crate) fn bitmask(x: M4) -> u32 {
    let bits = mask_and(x, [1, 2, 4, 8]);
    // SAFETY: `neon` is enabled for the whole build; the intrinsic takes values only.
    unsafe { vaddvq_u32(m(bits)) }
}
