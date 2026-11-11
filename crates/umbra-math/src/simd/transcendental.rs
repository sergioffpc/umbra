//! [`Vec4::exp`] and [`Vec4::pow`]: Umbra's own kernels, within 1 ULP of the
//! correctly rounded result in every lane (ADR 0022), where glibc per lane
//! costs several times more. The backend evaluates them in `f64`; this
//! facade gives `pow` C's values at zeros, infinities and NaNs.

use super::{Vec4, backend};
use crate::cmath;
use crate::float::floor;

/// What the exponent of [`Vec4::pow`] does to the sign of a negative base.
#[derive(Debug, Clone, Copy)]
enum Parity {
    /// An odd integer: the result keeps the sign of the base.
    Odd,
    /// An even integer: the result is positive.
    Even,
    /// Not an integer: a finite negative base gives NaN.
    Fraction,
}

impl Parity {
    /// The parity of the finite `e`.
    fn of(e: f32) -> Self {
        #[expect(clippy::float_cmp, reason = "an exact test for an integer")]
        let integer = floor(e) == e;
        if !integer {
            return Self::Fraction;
        }
        // Every `f32` of magnitude 2²⁴ or more is even.
        #[expect(
            clippy::cast_possible_truncation,
            reason = "an integer below 2²⁴ in magnitude"
        )]
        let odd = e.abs() < 16_777_216.0 && (e as i32) & 1 != 0;
        if odd { Self::Odd } else { Self::Even }
    }
}

impl Vec4 {
    /// `eˣ` per lane, within 1 ULP of the correctly rounded result: overflow
    /// gives `+∞`, underflow the subnormals and then `+0`, NaN stays NaN.
    ///
    /// pbrt-v4 `std::exp`, lane by lane.
    #[inline]
    #[must_use]
    pub fn exp(self) -> Self {
        Self(backend::exp(self.0))
    }

    /// Each lane raised to `e`, within 1 ULP of the correctly rounded
    /// result, with C `powf`'s values for zeros, infinities and NaNs. An
    /// exponent of 0 gives 1 and of 1 the lanes themselves, exactly; an
    /// infinite or NaN exponent falls back to `cmath::powf`.
    ///
    /// pbrt-v4 `std::pow`, lane by lane.
    #[must_use]
    pub fn pow(self, e: f32) -> Self {
        if e == 0.0 {
            return Self::splat(1.0);
        }
        #[expect(clippy::float_cmp, reason = "the exact identity")]
        if e == 1.0 {
            return self;
        }
        if !e.is_finite() {
            return Self(self.0.map(|x| cmath::powf(x, e)));
        }
        let r = self.abs().pow_magnitude(e);
        let nan = self.cmp_ne(self);
        match Parity::of(e) {
            Parity::Odd => nan.select(self, r.copy_sign(self)),
            Parity::Even => nan.select(self, r),
            Parity::Fraction => {
                let negative =
                    self.cmp_lt(Self::ZERO) & self.cmp_gt(Self::splat(f32::NEG_INFINITY));
                (nan | negative).select(Self::splat(f32::NAN), r)
            }
        }
    }

    /// `aᵉ` for the non-negative, non-NaN lanes `a` and a finite, non-zero
    /// `e`: the backend for finite positive lanes, `0` or `+∞` at zero and
    /// infinity.
    #[inline]
    fn pow_magnitude(self, e: f32) -> Self {
        let (at_zero, at_infinity) = if e > 0.0 {
            (0.0, f32::INFINITY)
        } else {
            (f32::INFINITY, 0.0)
        };
        let positive = Self(backend::pow_positive(self.0, e));
        let infinite = self.cmp_eq(Self::splat(f32::INFINITY));
        self.cmp_eq(Self::ZERO).select(
            Self::splat(at_zero),
            infinite.select(Self::splat(at_infinity), positive),
        )
    }
}
