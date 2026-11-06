//! The SSE backend of [`Vec4`](crate::Vec4) (ADR 0007): each operation is
//! one `core::arch` intrinsic on a `__m128`, chosen so that every lane
//! computes what the scalar operation computes on that lane:
//!
//! - `min_std`/`max_std` are `minps`/`maxps` with the operands swapped:
//!   `minps(b, a)` returns `b` when `b < a` and `a` otherwise (so `a` for a
//!   NaN in either), which is exactly C++ `std::min(a, b)`.
//! - `mul_add` is `vfmaddps`, always fused.
//! - `select` is `blendvps`.
//!
//! `__m128` and `[f32; 4]` have the same size, and every bit pattern is a
//! valid value of both, so the two are converted by `transmute`, which
//! compiles to nothing. The crate requires `x86-64-v3` (SSE2 to SSE4.2, AVX2,
//! FMA) on x86-64 and refuses to compile without it (`lib.rs`), so every CPU
//! this module is compiled for has its instructions.

use core::arch::x86_64::{
    __m128, __m128i, _mm_add_epi32, _mm_add_ps, _mm_and_ps, _mm_andnot_ps, _mm_cmpeq_ps,
    _mm_cmpge_ps, _mm_cmpgt_ps, _mm_cmple_ps, _mm_cmplt_ps, _mm_cmpneq_ps, _mm_div_ps, _mm_max_ps,
    _mm_min_ps, _mm_movehl_ps, _mm_movelh_ps, _mm_movemask_ps, _mm_mul_ps, _mm_or_ps,
    _mm_shuffle_ps, _mm_sqrt_ps, _mm_sub_ps, _mm_unpackhi_ps, _mm_unpacklo_ps, _mm_xor_ps,
};

use super::{F4, M4};

/// The lanes as a register.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): see the module docs"
)]
fn f(a: F4) -> __m128 {
    // SAFETY: `[f32; 4]` and `__m128` are both 16 bytes of four `f32` lanes,
    // and every bit pattern is a valid value of both.
    unsafe { core::mem::transmute::<F4, __m128>(a) }
}

/// The register as lanes.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): see the module docs"
)]
fn a(v: __m128) -> F4 {
    // SAFETY: `__m128` and `[f32; 4]` are both 16 bytes of four `f32` lanes,
    // and every bit pattern is a valid value of both.
    unsafe { core::mem::transmute::<__m128, F4>(v) }
}

/// The mask lanes as a register.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): see the module docs"
)]
fn m(a: M4) -> __m128 {
    // SAFETY: `[u32; 4]` and `__m128` are both 16 bytes of four 32-bit lanes,
    // and every bit pattern is a valid value of both.
    unsafe { core::mem::transmute::<M4, __m128>(a) }
}

/// The register as mask lanes.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): see the module docs"
)]
fn u(v: __m128) -> M4 {
    // SAFETY: `__m128` and `[u32; 4]` are both 16 bytes of four 32-bit lanes,
    // and every bit pattern is a valid value of both.
    unsafe { core::mem::transmute::<__m128, M4>(v) }
}

/// The lanes as an integer register.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): see the module docs"
)]
fn i(a: F4) -> __m128i {
    // SAFETY: `[f32; 4]` and `__m128i` are both 16 bytes of four 32-bit lanes,
    // and every bit pattern is a valid value of both.
    unsafe { core::mem::transmute::<F4, __m128i>(a) }
}

/// The integer register as lanes.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): see the module docs"
)]
fn ai(v: __m128i) -> F4 {
    // SAFETY: `__m128i` and `[f32; 4]` are both 16 bytes of four 32-bit lanes,
    // and every bit pattern is a valid value of both.
    unsafe { core::mem::transmute::<__m128i, F4>(v) }
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn add(x: F4, y: F4) -> F4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    a(unsafe { _mm_add_ps(f(x), f(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn sub(x: F4, y: F4) -> F4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    a(unsafe { _mm_sub_ps(f(x), f(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn mul(x: F4, y: F4) -> F4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    a(unsafe { _mm_mul_ps(f(x), f(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn div(x: F4, y: F4) -> F4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    a(unsafe { _mm_div_ps(f(x), f(y)) })
}

/// The sign bit flipped: `xorps` with `-0.0`.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn neg(x: F4) -> F4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    a(unsafe { _mm_xor_ps(f(x), f([-0.0; 4])) })
}

/// The sign bit cleared: `andnps` with `-0.0`.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn abs(x: F4) -> F4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    a(unsafe { _mm_andnot_ps(f([-0.0; 4]), f(x)) })
}

/// `vfmaddps`.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): the FMA instruction"
)]
pub(crate) fn mul_add(x: F4, y: F4, z: F4) -> F4 {
    use core::arch::x86_64::_mm_fmadd_ps;
    // SAFETY: the crate requires `x86-64-v3` on x86-64 (`lib.rs`), which
    // includes `fma`, so every CPU this binary runs on has the instruction.
    a(unsafe { _mm_fmadd_ps(f(x), f(y), f(z)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn sqrt(x: F4) -> F4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    a(unsafe { _mm_sqrt_ps(f(x)) })
}

/// `minps(y, x)`: `y` where `y < x`, else `x`, which is `std::min(x, y)`.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn min_std(x: F4, y: F4) -> F4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    a(unsafe { _mm_min_ps(f(y), f(x)) })
}

/// `maxps(y, x)`: `y` where `y > x`, else `x`, which is `std::max(x, y)`.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn max_std(x: F4, y: F4) -> F4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    a(unsafe { _mm_max_ps(f(y), f(x)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn cmp_eq(x: F4, y: F4) -> M4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    u(unsafe { _mm_cmpeq_ps(f(x), f(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn cmp_ne(x: F4, y: F4) -> M4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    u(unsafe { _mm_cmpneq_ps(f(x), f(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn cmp_lt(x: F4, y: F4) -> M4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    u(unsafe { _mm_cmplt_ps(f(x), f(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn cmp_le(x: F4, y: F4) -> M4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    u(unsafe { _mm_cmple_ps(f(x), f(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn cmp_gt(x: F4, y: F4) -> M4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    u(unsafe { _mm_cmpgt_ps(f(x), f(y)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn cmp_ge(x: F4, y: F4) -> M4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    u(unsafe { _mm_cmpge_ps(f(x), f(y)) })
}

/// `blendvps`: `t` where the mask is set, `fl` elsewhere.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): the SSE4.1 blend"
)]
pub(crate) fn select(mask: M4, t: F4, fl: F4) -> F4 {
    use core::arch::x86_64::_mm_blendv_ps;
    // SAFETY: the crate requires `x86-64-v3` on x86-64 (`lib.rs`), which
    // includes `sse4.1`, so every CPU this binary runs on has the instruction.
    a(unsafe { _mm_blendv_ps(f(fl), f(t), m(mask)) })
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
fn and(x: __m128, y: __m128) -> __m128 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    unsafe { _mm_and_ps(x, y) }
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
fn or(x: __m128, y: __m128) -> __m128 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    unsafe { _mm_or_ps(x, y) }
}

#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
fn xor(x: __m128, y: __m128) -> __m128 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    unsafe { _mm_xor_ps(x, y) }
}

/// `paddd`: the lanes as `i32`, added with wrapping.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn add_i32(x: F4, y: F4) -> F4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    ai(unsafe { _mm_add_epi32(i(x), i(y)) })
}

/// `unpcklps`: `[a0, b0, a1, b1]`.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn unpack_lo(x: F4, y: F4) -> F4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    a(unsafe { _mm_unpacklo_ps(f(x), f(y)) })
}

/// `unpckhps`: `[a2, b2, a3, b3]`.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn unpack_hi(x: F4, y: F4) -> F4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    a(unsafe { _mm_unpackhi_ps(f(x), f(y)) })
}

/// `movlhps`: `[a0, a1, b0, b1]`.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn move_lh(x: F4, y: F4) -> F4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    a(unsafe { _mm_movelh_ps(f(x), f(y)) })
}

/// `movhlps`: `[b2, b3, a2, a3]`.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn move_hl(x: F4, y: F4) -> F4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    a(unsafe { _mm_movehl_ps(f(x), f(y)) })
}

/// `shufps` with the immediate `MASK`.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
fn shuffle<const MASK: i32>(x: F4, y: F4) -> F4 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    a(unsafe { _mm_shuffle_ps::<MASK>(f(x), f(y)) })
}

/// The even lanes, `[a0, a2, b0, b2]`.
#[inline]
pub(crate) fn unzip_lo(x: F4, y: F4) -> F4 {
    shuffle::<0b10_00_10_00>(x, y)
}

/// The odd lanes, `[a1, a3, b1, b3]`.
#[inline]
pub(crate) fn unzip_hi(x: F4, y: F4) -> F4 {
    shuffle::<0b11_01_11_01>(x, y)
}

/// `shufps`: `[a1, a0, a3, a2]`.
#[inline]
pub(crate) fn swap_pairs(x: F4) -> F4 {
    shuffle::<0b10_11_00_01>(x, x)
}

#[inline]
pub(crate) fn mask_not(x: M4) -> M4 {
    u(xor(m(x), m([u32::MAX; 4])))
}

#[inline]
pub(crate) fn mask_and(x: M4, y: M4) -> M4 {
    u(and(m(x), m(y)))
}

#[inline]
pub(crate) fn mask_or(x: M4, y: M4) -> M4 {
    u(or(m(x), m(y)))
}

#[inline]
pub(crate) fn mask_xor(x: M4, y: M4) -> M4 {
    u(xor(m(x), m(y)))
}

/// `movmskps`: bit `i` set for each lane `i` whose sign bit is set.
#[inline]
#[expect(
    unsafe_code,
    reason = "aggressive optimisation (ADR 0007): one SSE2 intrinsic, see the module docs"
)]
pub(crate) fn bitmask(x: M4) -> u32 {
    // SAFETY: SSE2 is in the x86-64 baseline; the intrinsic takes values only.
    unsafe { _mm_movemask_ps(m(x)) }.cast_unsigned()
}
