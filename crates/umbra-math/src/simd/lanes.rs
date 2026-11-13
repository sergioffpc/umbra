//! The lane-by-lane backend of [`Vec4`](crate::Vec4), for arm64 and
//! `nvptx64` (ADR 0023, ADR 0028): four plain scalar lanes, each operation
//! the scalar operation applied lane by lane, which is the facade's contract
//! by construction. On arm64 LLVM vectorises the loops to NEON; the GPU
//! vectorises across threads, not within one.
//!
//! - `min_std`/`max_std` are [`crate::min`]/[`crate::max`], C++
//!   `std::min`/`std::max`.
//! - `mul_add` and `sqrt` are the scalar primitives: `fmadd` and `fsqrt` on
//!   arm64, `fma.rn.f32` and `sqrt.rn.f32` on `nvptx64`.
//! - `exp` and `pow_positive` are the target's `expf` and `powf` per lane:
//!   the C library's on arm64, within 1 ULP, and `libdevice`'s on `nvptx64`,
//!   within 2 and 4 ULP (`crate::libdevice`). Neither has the bits of the
//!   x86-64 backend's kernels; across machines, images are equal
//!   statistically only (ADR 0022).

use super::{F4, M4};
use crate::float::{fma, sqrt as scalar_sqrt};
use crate::math::{max, min};
use crate::{expf, powf};

/// `op` applied to each pair of lanes.
#[inline]
fn zip(x: F4, y: F4, op: impl Fn(f32, f32) -> f32) -> F4 {
    core::array::from_fn(|i| op(x[i], y[i]))
}

/// `test` of each pair of lanes, as a mask.
#[inline]
fn compare(x: F4, y: F4, test: impl Fn(f32, f32) -> bool) -> M4 {
    core::array::from_fn(|i| if test(x[i], y[i]) { u32::MAX } else { 0 })
}

/// `op` applied to each pair of mask lanes.
#[inline]
fn zip_mask(x: M4, y: M4, op: impl Fn(u32, u32) -> u32) -> M4 {
    core::array::from_fn(|i| op(x[i], y[i]))
}

#[inline]
pub(crate) fn add(x: F4, y: F4) -> F4 {
    zip(x, y, |a, b| a + b)
}

#[inline]
pub(crate) fn sub(x: F4, y: F4) -> F4 {
    zip(x, y, |a, b| a - b)
}

#[inline]
pub(crate) fn mul(x: F4, y: F4) -> F4 {
    zip(x, y, |a, b| a * b)
}

#[inline]
pub(crate) fn div(x: F4, y: F4) -> F4 {
    zip(x, y, |a, b| a / b)
}

/// The sign bit flipped.
#[inline]
pub(crate) fn neg(x: F4) -> F4 {
    x.map(|a| -a)
}

/// The sign bit cleared.
#[inline]
pub(crate) fn abs(x: F4) -> F4 {
    x.map(f32::abs)
}

/// The fused multiply-add per lane.
#[inline]
pub(crate) fn mul_add(x: F4, y: F4, z: F4) -> F4 {
    core::array::from_fn(|i| fma(x[i], y[i], z[i]))
}

/// The correctly rounded square root per lane.
#[inline]
pub(crate) fn sqrt(x: F4) -> F4 {
    x.map(scalar_sqrt)
}

/// `eˣ` per lane: the target's `expf`.
#[inline]
pub(crate) fn exp(x: F4) -> F4 {
    x.map(expf)
}

/// `xᵉ` per lane: the target's `powf`. The facade calls it for
/// finite positive `x` and finite `e` and replaces the other lanes.
#[inline]
pub(crate) fn pow_positive(x: F4, e: f32) -> F4 {
    x.map(|a| powf(a, e))
}

/// The magnitude of `x` with the sign bit of `s`.
#[inline]
pub(crate) fn copy_sign(x: F4, s: F4) -> F4 {
    zip(x, s, f32::copysign)
}

/// C++ `std::min(x, y)` per lane.
#[inline]
pub(crate) fn min_std(x: F4, y: F4) -> F4 {
    zip(x, y, min)
}

/// C++ `std::max(x, y)` per lane.
#[inline]
pub(crate) fn max_std(x: F4, y: F4) -> F4 {
    zip(x, y, max)
}

#[inline]
pub(crate) fn cmp_eq(x: F4, y: F4) -> M4 {
    compare(x, y, |a, b| a == b)
}

#[inline]
pub(crate) fn cmp_ne(x: F4, y: F4) -> M4 {
    #[expect(clippy::float_cmp, reason = "IEEE inequality, as the facade promises")]
    compare(x, y, |a, b| a != b)
}

#[inline]
pub(crate) fn cmp_lt(x: F4, y: F4) -> M4 {
    compare(x, y, |a, b| a < b)
}

#[inline]
pub(crate) fn cmp_le(x: F4, y: F4) -> M4 {
    compare(x, y, |a, b| a <= b)
}

#[inline]
pub(crate) fn cmp_gt(x: F4, y: F4) -> M4 {
    compare(x, y, |a, b| a > b)
}

#[inline]
pub(crate) fn cmp_ge(x: F4, y: F4) -> M4 {
    compare(x, y, |a, b| a >= b)
}

/// `t` where the mask is set, `f` elsewhere.
#[inline]
pub(crate) fn select(mask: M4, t: F4, f: F4) -> F4 {
    core::array::from_fn(|i| if mask[i] == 0 { f[i] } else { t[i] })
}

/// The lanes as `i32`, added with wrapping.
#[inline]
pub(crate) fn add_i32(x: F4, y: F4) -> F4 {
    zip(x, y, |a, b| {
        f32::from_bits(a.to_bits().wrapping_add(b.to_bits()))
    })
}

/// `[a0, b0, a1, b1]`.
#[inline]
pub(crate) fn unpack_lo(a: F4, b: F4) -> F4 {
    [a[0], b[0], a[1], b[1]]
}

/// `[a2, b2, a3, b3]`.
#[inline]
pub(crate) fn unpack_hi(a: F4, b: F4) -> F4 {
    [a[2], b[2], a[3], b[3]]
}

/// `[a0, a1, b0, b1]`.
#[inline]
pub(crate) fn move_lh(a: F4, b: F4) -> F4 {
    [a[0], a[1], b[0], b[1]]
}

/// `[b2, b3, a2, a3]`.
#[inline]
pub(crate) fn move_hl(a: F4, b: F4) -> F4 {
    [b[2], b[3], a[2], a[3]]
}

/// The even lanes, `[a0, a2, b0, b2]`.
#[inline]
pub(crate) fn unzip_lo(a: F4, b: F4) -> F4 {
    [a[0], a[2], b[0], b[2]]
}

/// The odd lanes, `[a1, a3, b1, b3]`.
#[inline]
pub(crate) fn unzip_hi(a: F4, b: F4) -> F4 {
    [a[1], a[3], b[1], b[3]]
}

/// `[a1, a0, a3, a2]`.
#[inline]
pub(crate) fn swap_pairs(a: F4) -> F4 {
    [a[1], a[0], a[3], a[2]]
}

#[inline]
pub(crate) fn mask_not(x: M4) -> M4 {
    x.map(|a| !a)
}

#[inline]
pub(crate) fn mask_and(x: M4, y: M4) -> M4 {
    zip_mask(x, y, |a, b| a & b)
}

#[inline]
pub(crate) fn mask_or(x: M4, y: M4) -> M4 {
    zip_mask(x, y, |a, b| a | b)
}

#[inline]
pub(crate) fn mask_xor(x: M4, y: M4) -> M4 {
    zip_mask(x, y, |a, b| a ^ b)
}

/// Bit `i` set for each lane `i` whose sign bit is set, as `movmskps`.
#[inline]
pub(crate) fn bitmask(x: M4) -> u32 {
    x.iter()
        .enumerate()
        .fold(0, |bits, (i, &lane)| bits | ((lane >> 31) << i))
}
