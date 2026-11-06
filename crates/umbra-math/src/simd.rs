//! The `Vec4`/`Mask4` SIMD facade over `wide` (ADR 0002, ADR 0007).
//!
//! Four `f32` lanes in one 128-bit register (SSE on x86-64, NEON on arm64),
//! the width of `SampledSpectrum` (ADR 0001). Each operation computes, lane
//! by lane, exactly what the scalar operation computes: the scalar code is
//! its reference and test oracle. Where `wide` leaves a result
//! platform-dependent, the facade pins it down:
//!
//! - `min`/`max` are selections with C++ `std::min`/`std::max` semantics
//!   (see [`crate::min`]), not the `minps`/`fmin` instructions, whose NaN
//!   rules differ between x86-64 and arm64.
//! - `mul_add` is fused on every build: without a hardware FMA target
//!   feature, `wide` would round twice, so the facade falls back to the
//!   correctly rounded `libm::fmaf` per lane.
//! - `sqrt` falls back to `libm::sqrtf` per lane where `wide` has no
//!   hardware square root.
//! - There are no reciprocal or reciprocal-square-root estimates, and no
//!   horizontal operations whose order depends on the platform.
//!
//! NaN payloads produced by arithmetic on non-NaN inputs (the default NaN)
//! differ between CPU vendors in the scalar code too; they are not part of
//! the contract.

use core::ops::{
    Add, AddAssign, BitAnd, BitOr, BitXor, Div, DivAssign, Index, Mul, MulAssign, Neg, Not, Sub,
    SubAssign,
};

use wide::f32x4;

use crate::math::{max, min};

/// Four `f32` lanes.
#[derive(Debug, Clone, Copy, Default)]
pub struct Vec4(f32x4);

/// The per-lane result of a [`Vec4`] comparison: all bits set for true, all
/// clear for false.
#[derive(Debug, Clone, Copy, Default)]
pub struct Mask4(f32x4);

impl Vec4 {
    /// All lanes zero.
    pub const ZERO: Self = Self(f32x4::ZERO);

    /// The lanes `[a, b, c, d]`.
    #[inline]
    #[must_use]
    pub const fn new(a: f32, b: f32, c: f32, d: f32) -> Self {
        Self(f32x4::new([a, b, c, d]))
    }

    /// `v` in every lane.
    #[inline]
    #[must_use]
    pub const fn splat(v: f32) -> Self {
        Self(f32x4::splat(v))
    }

    /// The lanes as an array.
    #[inline]
    #[must_use]
    pub const fn to_array(self) -> [f32; 4] {
        *self.0.as_array()
    }

    /// `self · b + c` per lane, with one rounding.
    #[inline]
    #[must_use]
    pub fn mul_add(self, b: Self, c: Self) -> Self {
        #[cfg(any(
            all(target_arch = "x86_64", target_feature = "fma"),
            all(target_arch = "aarch64", target_feature = "neon")
        ))]
        {
            Self(self.0.mul_add(b.0, c.0))
        }
        #[cfg(not(any(
            all(target_arch = "x86_64", target_feature = "fma"),
            all(target_arch = "aarch64", target_feature = "neon")
        )))]
        {
            self.zip3(b, c, crate::fma)
        }
    }

    /// The square root per lane, correctly rounded.
    #[inline]
    #[must_use]
    pub fn sqrt(self) -> Self {
        #[cfg(any(
            all(target_arch = "x86_64", target_feature = "sse"),
            all(target_arch = "aarch64", target_feature = "neon")
        ))]
        {
            Self(self.0.sqrt())
        }
        #[cfg(not(any(
            all(target_arch = "x86_64", target_feature = "sse"),
            all(target_arch = "aarch64", target_feature = "neon")
        )))]
        {
            self.map(libm::sqrtf)
        }
    }

    /// The absolute value per lane: the sign bit cleared.
    #[inline]
    #[must_use]
    pub fn abs(self) -> Self {
        Self(self.0.abs())
    }

    /// Per lane, `other` if `other < self`, else `self`: C++
    /// `std::min(self, other)`. A NaN in `other` yields `self`.
    #[inline]
    #[must_use]
    pub fn min(self, other: Self) -> Self {
        other.cmp_lt(self).select(other, self)
    }

    /// Per lane, `other` if `self < other`, else `self`: C++
    /// `std::max(self, other)`. A NaN in `other` yields `self`, so
    /// `t0.max(t_near)` ignores a NaN slab bound as pbrt-v4's ray–box test
    /// does.
    #[inline]
    #[must_use]
    pub fn max(self, other: Self) -> Self {
        self.cmp_lt(other).select(other, self)
    }

    /// `self == other` per lane (false for NaN).
    #[inline]
    #[must_use]
    pub fn cmp_eq(self, other: Self) -> Mask4 {
        Mask4(self.0.simd_eq(other.0))
    }

    /// `self != other` per lane (true for NaN).
    #[inline]
    #[must_use]
    pub fn cmp_ne(self, other: Self) -> Mask4 {
        Mask4(self.0.simd_ne(other.0))
    }

    /// `self < other` per lane.
    #[inline]
    #[must_use]
    pub fn cmp_lt(self, other: Self) -> Mask4 {
        Mask4(self.0.simd_lt(other.0))
    }

    /// `self <= other` per lane.
    #[inline]
    #[must_use]
    pub fn cmp_le(self, other: Self) -> Mask4 {
        Mask4(self.0.simd_le(other.0))
    }

    /// `self > other` per lane.
    #[inline]
    #[must_use]
    pub fn cmp_gt(self, other: Self) -> Mask4 {
        Mask4(self.0.simd_gt(other.0))
    }

    /// `self >= other` per lane.
    #[inline]
    #[must_use]
    pub fn cmp_ge(self, other: Self) -> Mask4 {
        Mask4(self.0.simd_ge(other.0))
    }

    /// `((l0 + l1) + l2) + l3`.
    #[inline]
    #[must_use]
    pub fn reduce_add(self) -> f32 {
        let [a, b, c, d] = self.to_array();
        ((a + b) + c) + d
    }

    /// `min(min(min(l0, l1), l2), l3)`, with [`crate::min`] semantics.
    #[inline]
    #[must_use]
    pub fn reduce_min(self) -> f32 {
        let [a, b, c, d] = self.to_array();
        min(min(min(a, b), c), d)
    }

    /// `max(max(max(l0, l1), l2), l3)`, with [`crate::max`] semantics.
    #[inline]
    #[must_use]
    pub fn reduce_max(self) -> f32 {
        let [a, b, c, d] = self.to_array();
        max(max(max(a, b), c), d)
    }

    /// `f` applied to each lane.
    #[cfg(not(any(
        all(target_arch = "x86_64", target_feature = "sse"),
        all(target_arch = "aarch64", target_feature = "neon")
    )))]
    fn map(self, f: impl Fn(f32) -> f32) -> Self {
        Self::from(self.to_array().map(f))
    }

    /// `f` applied lane by lane to `self`, `b` and `c`.
    #[cfg(not(any(
        all(target_arch = "x86_64", target_feature = "fma"),
        all(target_arch = "aarch64", target_feature = "neon")
    )))]
    fn zip3(self, b: Self, c: Self, f: impl Fn(f32, f32, f32) -> f32) -> Self {
        let (a, b, c) = (self.to_array(), b.to_array(), c.to_array());
        Self::from(core::array::from_fn(|i| f(a[i], b[i], c[i])))
    }
}

impl From<[f32; 4]> for Vec4 {
    #[inline]
    fn from(a: [f32; 4]) -> Self {
        Self(f32x4::new(a))
    }
}

impl From<Vec4> for [f32; 4] {
    #[inline]
    fn from(v: Vec4) -> Self {
        v.to_array()
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
        &self.0.as_array()[i]
    }
}

/// The IEEE operations, which are correctly rounded and so identical in
/// every lane and in scalar code.
macro_rules! impl_arithmetic {
    ($($trait:ident $method:ident $assign_trait:ident $assign:ident $op:tt),*) => {$(
        impl $trait for Vec4 {
            type Output = Self;
            #[inline]
            fn $method(self, o: Self) -> Self {
                Self(self.0 $op o.0)
            }
        }

        impl $trait<f32> for Vec4 {
            type Output = Self;
            #[inline]
            fn $method(self, s: f32) -> Self {
                self $op Self::splat(s)
            }
        }

        impl $assign_trait for Vec4 {
            #[inline]
            fn $assign(&mut self, o: Self) {
                *self = *self $op o;
            }
        }

        impl $assign_trait<f32> for Vec4 {
            #[inline]
            fn $assign(&mut self, s: f32) {
                *self = *self $op s;
            }
        }
    )*};
}
impl_arithmetic!(
    Add add AddAssign add_assign +,
    Sub sub SubAssign sub_assign -,
    Mul mul MulAssign mul_assign *,
    Div div DivAssign div_assign /
);

impl Mul<Vec4> for f32 {
    type Output = Vec4;
    #[inline]
    fn mul(self, v: Vec4) -> Vec4 {
        Vec4::splat(self) * v
    }
}

impl Neg for Vec4 {
    type Output = Self;
    /// The sign bit flipped, as scalar negation.
    #[inline]
    fn neg(self) -> Self {
        Self(-self.0)
    }
}

impl Mask4 {
    /// `if_true` in the lanes where the mask is set, `if_false` elsewhere.
    #[inline]
    #[must_use]
    pub fn select(self, if_true: Vec4, if_false: Vec4) -> Vec4 {
        Vec4(self.0.select(if_true.0, if_false.0))
    }

    /// Whether any lane is set.
    #[inline]
    #[must_use]
    pub fn any(self) -> bool {
        self.0.any()
    }

    /// Whether every lane is set.
    #[inline]
    #[must_use]
    pub fn all(self) -> bool {
        self.0.all()
    }

    /// Whether no lane is set.
    #[inline]
    #[must_use]
    pub fn none(self) -> bool {
        !self.0.any()
    }

    /// The lanes as booleans.
    #[must_use]
    pub fn to_array(self) -> [bool; 4] {
        let bits = self.0.to_bitmask();
        core::array::from_fn(|i| bits & (1 << i) != 0)
    }
}

impl From<[bool; 4]> for Mask4 {
    fn from(lanes: [bool; 4]) -> Self {
        let set = f32::from_bits(u32::MAX);
        Self(f32x4::new(lanes.map(|l| if l { set } else { 0.0 })))
    }
}

impl Not for Mask4 {
    type Output = Self;
    #[inline]
    fn not(self) -> Self {
        Self(!self.0)
    }
}

impl BitAnd for Mask4 {
    type Output = Self;
    #[inline]
    fn bitand(self, o: Self) -> Self {
        Self(self.0 & o.0)
    }
}

impl BitOr for Mask4 {
    type Output = Self;
    #[inline]
    fn bitor(self, o: Self) -> Self {
        Self(self.0 | o.0)
    }
}

impl BitXor for Mask4 {
    type Output = Self;
    #[inline]
    fn bitxor(self, o: Self) -> Self {
        Self(self.0 ^ o.0)
    }
}
