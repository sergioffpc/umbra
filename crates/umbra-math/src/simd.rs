//! The `Vec4`/`Mask4` SIMD facade (ADR 0002, ADR 0007).
//!
//! Four `f32` lanes in one 128-bit register (SSE on x86-64, NEON on arm64),
//! the width of `SampledSpectrum` (ADR 0001). Each operation computes, lane
//! by lane, exactly what the scalar operation computes: the scalar code is
//! its reference and test oracle (`tests/umbra_math/simd_test.rs`). There is
//! one backend per supported architecture: `x86_64` (SSE, FMA) and `aarch64`
//! (NEON). Where the hardware
//! leaves a result platform-dependent, the facade pins it down:
//!
//! - `min`/`max` have C++ `std::min`/`std::max` semantics (see
//!   [`crate::min`]): `minps`/`maxps` with swapped operands on x86-64, a
//!   compare and a select on arm64, never `fmin`/`fminnm`, whose NaN rules
//!   differ between the two.
//! - `mul_add` is always fused: `vfmaddps` on x86-64, `fmla` on arm64,
//!   never a multiply and an add.
//! - There are no reciprocal or reciprocal-square-root estimates, and no
//!   horizontal operations whose order depends on the platform.
//!
//! NaN payloads produced by arithmetic on non-NaN inputs (the default NaN)
//! differ between CPU vendors in the scalar code too; they are not part of
//! the contract.
//!
//! The lanes are stored as a 16-byte-aligned `[f32; 4]`, so constructors are
//! `const` and the layout is explicit; each backend moves them into a
//! register for the operation and back, which costs nothing once inlined.

use core::ops::{
    Add, AddAssign, BitAnd, BitOr, BitXor, Div, DivAssign, Index, Mul, MulAssign, Neg, Not, Sub,
    SubAssign,
};

#[cfg(all(target_arch = "aarch64", target_feature = "neon"))]
mod aarch64;
#[cfg(target_arch = "x86_64")]
mod x86_64;

#[cfg(target_arch = "x86_64")]
use x86_64 as backend;

#[cfg(all(target_arch = "aarch64", target_feature = "neon"))]
use aarch64 as backend;

/// Four `f32` lanes, as every backend takes and returns them.
type F4 = [f32; 4];
/// Four lane masks: all bits set for true, all clear for false.
type M4 = [u32; 4];

use crate::math::{max, min};

/// Four `f32` lanes.
#[derive(Debug, Clone, Copy, Default)]
#[repr(C, align(16))]
pub struct Vec4([f32; 4]);

/// The per-lane result of a [`Vec4`] comparison: all bits set for true, all
/// clear for false.
#[derive(Debug, Clone, Copy, Default)]
#[repr(C, align(16))]
pub struct Mask4([u32; 4]);

impl Vec4 {
    /// All lanes zero.
    pub const ZERO: Self = Self([0.0; 4]);

    /// The lanes `[a, b, c, d]`.
    #[inline]
    #[must_use]
    pub const fn new(a: f32, b: f32, c: f32, d: f32) -> Self {
        Self([a, b, c, d])
    }

    /// `v` in every lane.
    #[inline]
    #[must_use]
    pub const fn splat(v: f32) -> Self {
        Self([v; 4])
    }

    /// The lanes as an array.
    #[inline]
    #[must_use]
    pub const fn to_array(self) -> [f32; 4] {
        self.0
    }

    /// The lanes from their bit patterns.
    #[inline]
    #[must_use]
    pub(crate) const fn from_bits(bits: [u32; 4]) -> Self {
        let [a, b, c, d] = bits;
        Self([
            f32::from_bits(a),
            f32::from_bits(b),
            f32::from_bits(c),
            f32::from_bits(d),
        ])
    }

    /// Lane `I`.
    #[inline]
    #[must_use]
    pub(crate) const fn lane<const I: usize>(self) -> f32 {
        const { assert!(I < 4, "a Vec4 has four lanes") };
        self.0[I]
    }

    /// `self · b + c` per lane, with one rounding.
    #[inline]
    #[must_use]
    pub fn mul_add(self, b: Self, c: Self) -> Self {
        Self(backend::mul_add(self.0, b.0, c.0))
    }

    /// The square root per lane, correctly rounded.
    #[inline]
    #[must_use]
    pub fn sqrt(self) -> Self {
        Self(backend::sqrt(self.0))
    }

    /// The absolute value per lane: the sign bit cleared.
    #[inline]
    #[must_use]
    pub fn abs(self) -> Self {
        Self(backend::abs(self.0))
    }

    /// Per lane, `other` if `other < self`, else `self`: C++
    /// `std::min(self, other)`. A NaN in `other` yields `self`.
    #[inline]
    #[must_use]
    pub fn min(self, other: Self) -> Self {
        Self(backend::min_std(self.0, other.0))
    }

    /// Per lane, `other` if `self < other`, else `self`: C++
    /// `std::max(self, other)`. A NaN in `other` yields `self`, so
    /// `t0.max(t_near)` ignores a NaN slab bound as pbrt-v4's ray–box test
    /// does.
    #[inline]
    #[must_use]
    pub fn max(self, other: Self) -> Self {
        Self(backend::max_std(self.0, other.0))
    }

    /// `self == other` per lane (false for NaN).
    #[inline]
    #[must_use]
    pub fn cmp_eq(self, other: Self) -> Mask4 {
        Mask4(backend::cmp_eq(self.0, other.0))
    }

    /// `self != other` per lane (true for NaN).
    #[inline]
    #[must_use]
    pub fn cmp_ne(self, other: Self) -> Mask4 {
        Mask4(backend::cmp_ne(self.0, other.0))
    }

    /// `self < other` per lane.
    #[inline]
    #[must_use]
    pub fn cmp_lt(self, other: Self) -> Mask4 {
        Mask4(backend::cmp_lt(self.0, other.0))
    }

    /// `self <= other` per lane.
    #[inline]
    #[must_use]
    pub fn cmp_le(self, other: Self) -> Mask4 {
        Mask4(backend::cmp_le(self.0, other.0))
    }

    /// `self > other` per lane.
    #[inline]
    #[must_use]
    pub fn cmp_gt(self, other: Self) -> Mask4 {
        Mask4(backend::cmp_gt(self.0, other.0))
    }

    /// `self >= other` per lane.
    #[inline]
    #[must_use]
    pub fn cmp_ge(self, other: Self) -> Mask4 {
        Mask4(backend::cmp_ge(self.0, other.0))
    }

    /// `((l0 + l1) + l2) + l3`.
    #[inline]
    #[must_use]
    pub fn reduce_add(self) -> f32 {
        let [a, b, c, d] = self.0;
        ((a + b) + c) + d
    }

    /// `min(min(min(l0, l1), l2), l3)`, with [`crate::min`] semantics.
    #[inline]
    #[must_use]
    pub fn reduce_min(self) -> f32 {
        let [a, b, c, d] = self.0;
        min(min(min(a, b), c), d)
    }

    /// `max(max(max(l0, l1), l2), l3)`, with [`crate::max`] semantics.
    #[inline]
    #[must_use]
    pub fn reduce_max(self) -> f32 {
        let [a, b, c, d] = self.0;
        max(max(max(a, b), c), d)
    }

    /// The lanes as `i32`, added with wrapping: `self` moved by `delta`
    /// ULPs, the step of `next_float_up`/`next_float_down`.
    #[inline]
    #[must_use]
    pub(crate) fn add_i32(self, delta: Self) -> Self {
        Self(backend::add_i32(self.0, delta.0))
    }

    /// `[a0, b0, a1, b1]` of `a = self`.
    #[inline]
    #[must_use]
    pub(crate) fn unpack_lo(self, b: Self) -> Self {
        Self(backend::unpack_lo(self.0, b.0))
    }

    /// `[a2, b2, a3, b3]` of `a = self`.
    #[inline]
    #[must_use]
    pub(crate) fn unpack_hi(self, b: Self) -> Self {
        Self(backend::unpack_hi(self.0, b.0))
    }

    /// `[a0, a1, b0, b1]` of `a = self`.
    #[inline]
    #[must_use]
    pub(crate) fn move_lh(self, b: Self) -> Self {
        Self(backend::move_lh(self.0, b.0))
    }

    /// `[b2, b3, a2, a3]` of `a = self`.
    #[inline]
    #[must_use]
    pub(crate) fn move_hl(self, b: Self) -> Self {
        Self(backend::move_hl(self.0, b.0))
    }

    /// The even lanes of both, `[a0, a2, b0, b2]` of `a = self`.
    #[inline]
    #[must_use]
    pub(crate) fn unzip_lo(self, b: Self) -> Self {
        Self(backend::unzip_lo(self.0, b.0))
    }

    /// The odd lanes of both, `[a1, a3, b1, b3]` of `a = self`.
    #[inline]
    #[must_use]
    pub(crate) fn unzip_hi(self, b: Self) -> Self {
        Self(backend::unzip_hi(self.0, b.0))
    }

    /// `[a1, a0, a3, a2]` of `a = self`: the lanes of each pair swapped.
    #[inline]
    #[must_use]
    pub(crate) fn swap_pairs(self) -> Self {
        Self(backend::swap_pairs(self.0))
    }

    /// The rows of a 4 × 4 matrix as its columns.
    #[inline]
    #[must_use]
    pub(crate) fn transpose(rows: [Self; 4]) -> [Self; 4] {
        let [r0, r1, r2, r3] = rows;
        let (t0, t1) = (r0.unpack_lo(r1), r2.unpack_lo(r3));
        let (t2, t3) = (r0.unpack_hi(r1), r2.unpack_hi(r3));
        [
            t0.move_lh(t1),
            t1.move_hl(t0),
            t2.move_lh(t3),
            t3.move_hl(t2),
        ]
    }
}

impl From<[f32; 4]> for Vec4 {
    #[inline]
    fn from(a: [f32; 4]) -> Self {
        Self(a)
    }
}

impl From<Vec4> for [f32; 4] {
    #[inline]
    fn from(v: Vec4) -> Self {
        v.0
    }
}

impl PartialEq for Vec4 {
    /// Lane-wise IEEE equality: NaN lanes are never equal.
    fn eq(&self, other: &Self) -> bool {
        self.cmp_eq(*other).all()
    }
}

impl Index<usize> for Vec4 {
    type Output = f32;
    /// Lane `i`.
    #[inline]
    fn index(&self, i: usize) -> &f32 {
        &self.0[i]
    }
}

/// The IEEE operations, which are correctly rounded and so identical in
/// every lane and in scalar code.
macro_rules! impl_arithmetic {
    ($($trait:ident $method:ident $assign_trait:ident $assign:ident $backend:ident),*) => {$(
        impl $trait for Vec4 {
            type Output = Self;
            #[inline]
            fn $method(self, o: Self) -> Self {
                Self(backend::$backend(self.0, o.0))
            }
        }

        impl $trait<f32> for Vec4 {
            type Output = Self;
            #[inline]
            fn $method(self, s: f32) -> Self {
                Self(backend::$backend(self.0, [s; 4]))
            }
        }

        impl $assign_trait for Vec4 {
            #[inline]
            fn $assign(&mut self, o: Self) {
                *self = Self(backend::$backend(self.0, o.0));
            }
        }

        impl $assign_trait<f32> for Vec4 {
            #[inline]
            fn $assign(&mut self, s: f32) {
                *self = Self(backend::$backend(self.0, [s; 4]));
            }
        }
    )*};
}
impl_arithmetic!(
    Add add AddAssign add_assign add,
    Sub sub SubAssign sub_assign sub,
    Mul mul MulAssign mul_assign mul,
    Div div DivAssign div_assign div
);

impl Mul<Vec4> for f32 {
    type Output = Vec4;
    #[inline]
    fn mul(self, v: Vec4) -> Vec4 {
        Vec4(backend::mul([self; 4], v.0))
    }
}

impl Neg for Vec4 {
    type Output = Self;
    /// The sign bit flipped, as scalar negation.
    #[inline]
    fn neg(self) -> Self {
        Self(backend::neg(self.0))
    }
}

impl Mask4 {
    /// The lanes from booleans.
    #[inline]
    #[must_use]
    pub(crate) const fn new(lanes: [bool; 4]) -> Self {
        let [a, b, c, d] = lanes;
        Self([
            if a { u32::MAX } else { 0 },
            if b { u32::MAX } else { 0 },
            if c { u32::MAX } else { 0 },
            if d { u32::MAX } else { 0 },
        ])
    }

    /// `if_true` in the lanes where the mask is set, `if_false` elsewhere.
    #[inline]
    #[must_use]
    pub fn select(self, if_true: Vec4, if_false: Vec4) -> Vec4 {
        Vec4(backend::select(self.0, if_true.0, if_false.0))
    }

    /// `if_true` in the lanes where the mask is set, `if_false` elsewhere,
    /// for masks.
    #[inline]
    #[must_use]
    pub(crate) fn select_mask(self, if_true: Self, if_false: Self) -> Self {
        (self & if_true) | (!self & if_false)
    }

    /// Whether any lane is set.
    #[inline]
    #[must_use]
    pub fn any(self) -> bool {
        self.bitmask() != 0
    }

    /// Whether every lane is set.
    #[inline]
    #[must_use]
    pub fn all(self) -> bool {
        self.bitmask() == 0b1111
    }

    /// Whether no lane is set.
    #[inline]
    #[must_use]
    pub fn none(self) -> bool {
        self.bitmask() == 0
    }

    /// Bit `i` set for each lane `i` that is set.
    #[inline]
    #[must_use]
    pub(crate) fn bitmask(self) -> u32 {
        backend::bitmask(self.0)
    }

    /// The lanes as booleans.
    #[must_use]
    pub fn to_array(self) -> [bool; 4] {
        let bits = self.bitmask();
        core::array::from_fn(|i| bits & (1 << i) != 0)
    }
}

impl From<[bool; 4]> for Mask4 {
    fn from(lanes: [bool; 4]) -> Self {
        Self::new(lanes)
    }
}

impl Not for Mask4 {
    type Output = Self;
    #[inline]
    fn not(self) -> Self {
        Self(backend::mask_not(self.0))
    }
}

impl BitAnd for Mask4 {
    type Output = Self;
    #[inline]
    fn bitand(self, o: Self) -> Self {
        Self(backend::mask_and(self.0, o.0))
    }
}

impl BitOr for Mask4 {
    type Output = Self;
    #[inline]
    fn bitor(self, o: Self) -> Self {
        Self(backend::mask_or(self.0, o.0))
    }
}

impl BitXor for Mask4 {
    type Output = Self;
    #[inline]
    fn bitxor(self, o: Self) -> Self {
        Self(backend::mask_xor(self.0, o.0))
    }
}
