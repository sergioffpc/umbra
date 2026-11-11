//! Tristimulus colours, `RGB` and `XYZ`, and the sigmoid polynomial an RGB
//! becomes as a spectrum (pbrt-v4 `src/pbrt/util/color.h`).

use core::ops::{
    Add, AddAssign, Div, DivAssign, Index, IndexMut, Mul, MulAssign, Neg, Sub, SubAssign,
};

use umbra_math::{Point2f, evaluate_polynomial, max, sqr, sqrt};

/// The lane-wise arithmetic pbrt-v4 gives `RGB` and `XYZ`, with its
/// `DCHECK`s on scalar operands.
macro_rules! tristimulus_ops {
    ($t:ident { $a:ident, $b:ident, $c:ident }) => {
        impl $t {
            /// The mean of the three components.
            #[must_use]
            pub fn average(&self) -> f32 {
                (self.$a + self.$b + self.$c) / 3.0
            }

            /// Each component clamped to `[min, max]`.
            #[must_use]
            pub fn clamp(self, min: f32, max: f32) -> Self {
                self.map(|v| umbra_math::clamp(v, min, max))
            }

            /// Each component clamped below at 0, as C++ `std::max(0, v)`.
            #[must_use]
            pub fn clamp_zero(self) -> Self {
                self.map(|v| max(0.0, v))
            }

            /// `(1 − t) · a + t · b`.
            #[must_use]
            pub fn lerp(t: f32, a: Self, b: Self) -> Self {
                (1.0 - t) * a + t * b
            }

            /// `f` applied to each component.
            fn map(self, mut f: impl FnMut(f32) -> f32) -> Self {
                Self::new(f(self.$a), f(self.$b), f(self.$c))
            }

            /// `f` applied to each pair of components.
            fn zip(self, o: Self, mut f: impl FnMut(f32, f32) -> f32) -> Self {
                Self::new(f(self.$a, o.$a), f(self.$b, o.$b), f(self.$c, o.$c))
            }
        }

        impl From<[f32; 3]> for $t {
            fn from([$a, $b, $c]: [f32; 3]) -> Self {
                Self::new($a, $b, $c)
            }
        }

        impl From<$t> for [f32; 3] {
            fn from(v: $t) -> Self {
                [v.$a, v.$b, v.$c]
            }
        }

        impl Index<usize> for $t {
            type Output = f32;
            /// Component `c`, in order.
            ///
            /// # Panics
            ///
            /// If `c > 2`.
            fn index(&self, c: usize) -> &f32 {
                match c {
                    0 => &self.$a,
                    1 => &self.$b,
                    2 => &self.$c,
                    _ => panic!("component {c} of a tristimulus colour"),
                }
            }
        }

        impl IndexMut<usize> for $t {
            /// Component `c`, in order.
            ///
            /// # Panics
            ///
            /// If `c > 2`.
            fn index_mut(&mut self, c: usize) -> &mut f32 {
                match c {
                    0 => &mut self.$a,
                    1 => &mut self.$b,
                    2 => &mut self.$c,
                    _ => panic!("component {c} of a tristimulus colour"),
                }
            }
        }

        impl Add for $t {
            type Output = Self;
            fn add(self, o: Self) -> Self {
                self.zip(o, |a, b| a + b)
            }
        }

        impl AddAssign for $t {
            fn add_assign(&mut self, o: Self) {
                *self = *self + o;
            }
        }

        impl Sub for $t {
            type Output = Self;
            fn sub(self, o: Self) -> Self {
                self.zip(o, |a, b| a - b)
            }
        }

        impl SubAssign for $t {
            fn sub_assign(&mut self, o: Self) {
                *self = *self - o;
            }
        }

        impl Sub<$t> for f32 {
            type Output = $t;
            fn sub(self, v: $t) -> $t {
                v.map(|c| self - c)
            }
        }

        impl Mul for $t {
            type Output = Self;
            fn mul(self, o: Self) -> Self {
                self.zip(o, |a, b| a * b)
            }
        }

        impl MulAssign for $t {
            fn mul_assign(&mut self, o: Self) {
                *self = *self * o;
            }
        }

        impl Mul<f32> for $t {
            type Output = Self;
            fn mul(self, a: f32) -> Self {
                debug_assert!(!a.is_nan(), "{self:?} * NaN");
                self.map(|c| a * c)
            }
        }

        impl MulAssign<f32> for $t {
            fn mul_assign(&mut self, a: f32) {
                debug_assert!(!a.is_nan(), "{self:?} *= NaN");
                *self = self.map(|c| c * a);
            }
        }

        impl Mul<$t> for f32 {
            type Output = $t;
            fn mul(self, v: $t) -> $t {
                v * self
            }
        }

        impl Div for $t {
            type Output = Self;
            fn div(self, o: Self) -> Self {
                self.zip(o, |a, b| a / b)
            }
        }

        impl DivAssign for $t {
            fn div_assign(&mut self, o: Self) {
                *self = *self / o;
            }
        }

        impl Div<f32> for $t {
            type Output = Self;
            fn div(mut self, a: f32) -> Self {
                self /= a;
                self
            }
        }

        impl DivAssign<f32> for $t {
            fn div_assign(&mut self, a: f32) {
                debug_assert!(a != 0.0 && !a.is_nan(), "{self:?} / {a}");
                *self = self.map(|c| c / a);
            }
        }

        impl Neg for $t {
            type Output = Self;
            fn neg(self) -> Self {
                self.map(|c| -c)
            }
        }
    };
}

/// A colour in the linear RGB of some [`RGBColorSpace`](crate::RGBColorSpace).
///
/// PBR 4e §4.6.4, pbrt-v4 `RGB`.
#[expect(
    clippy::upper_case_acronyms,
    reason = "pbrt-v4's name, as in RGBColorSpace"
)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[repr(C)]
pub struct RGB {
    /// Red.
    pub r: f32,
    /// Green.
    pub g: f32,
    /// Blue.
    pub b: f32,
}

impl RGB {
    /// `(r, g, b)`.
    #[must_use]
    pub const fn new(r: f32, g: f32, b: f32) -> Self {
        Self { r, g, b }
    }

    /// The component-wise maximum, with C++ `std::max`.
    ///
    /// pbrt-v4 `max(RGB, RGB)`.
    #[must_use]
    pub fn max(self, o: Self) -> Self {
        self.zip(o, max)
    }
}

tristimulus_ops!(RGB { r, g, b });

/// A colour in CIE 1931 XYZ.
///
/// PBR 4e §4.6.1, pbrt-v4 `XYZ`.
#[expect(
    clippy::upper_case_acronyms,
    reason = "pbrt-v4's name, as the colour space's"
)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[repr(C)]
pub struct XYZ {
    /// X.
    pub x: f32,
    /// Y, the luminance.
    pub y: f32,
    /// Z.
    pub z: f32,
}

impl XYZ {
    /// `(x, y, z)`.
    #[must_use]
    pub const fn new(x: f32, y: f32, z: f32) -> Self {
        Self { x, y, z }
    }

    /// The chromaticity `(x, y) = (X, Y) / (X + Y + Z)`.
    ///
    /// pbrt-v4 `XYZ::xy`.
    #[must_use]
    pub fn xy(&self) -> Point2f {
        Point2f::new(
            self.x / (self.x + self.y + self.z),
            self.y / (self.x + self.y + self.z),
        )
    }

    /// The XYZ of chromaticity `xy` with luminance `y`; black if `xy.y` is
    /// 0.
    ///
    /// pbrt-v4 `XYZ::FromxyY`.
    #[must_use]
    pub fn from_xy_y(xy: Point2f, y: f32) -> Self {
        if xy.y == 0.0 {
            return Self::default();
        }
        Self::new(xy.x * y / xy.y, y, (1.0 - xy.x - xy.y) * y / xy.y)
    }
}

tristimulus_ops!(XYZ { x, y, z });

/// The spectrum an RGB becomes: the sigmoid `s(x) = ½ + x / (2√(1 + x²))`
/// of a quadratic `c0 λ² + c1 λ + c2` in λ in nm, smooth and bounded to
/// `[0, 1]` (Jakob and Hanika 2019).
///
/// PBR 4e §4.6.6, pbrt-v4 `RGBSigmoidPolynomial`.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[repr(C)]
pub struct RGBSigmoidPolynomial {
    c0: f32,
    c1: f32,
    c2: f32,
}

// The host copies arena slices of these into GPU buffers byte for byte
// (ADR 0028, arc42 §8.12): their layout is the same on `x86_64` and `nvptx64`.
const _: () = {
    use core::mem::offset_of;

    assert!(size_of::<RGB>() == 12, "`RGB` is 12 bytes, with no padding");
    assert!(offset_of!(RGB, r) == 0, "`RGB::r` is at byte 0");
    assert!(offset_of!(RGB, g) == 4, "`RGB::g` is at byte 4");
    assert!(offset_of!(RGB, b) == 8, "`RGB::b` is at byte 8");
    assert!(size_of::<XYZ>() == 12, "`XYZ` is 12 bytes, with no padding");
    assert!(offset_of!(XYZ, x) == 0, "`XYZ::x` is at byte 0");
    assert!(offset_of!(XYZ, y) == 4, "`XYZ::y` is at byte 4");
    assert!(offset_of!(XYZ, z) == 8, "`XYZ::z` is at byte 8");
    assert!(
        size_of::<RGBSigmoidPolynomial>() == 12,
        "`RGBSigmoidPolynomial` is 12 bytes, with no padding"
    );
    assert!(
        offset_of!(RGBSigmoidPolynomial, c0) == 0,
        "`RGBSigmoidPolynomial::c0` is at byte 0"
    );
    assert!(
        offset_of!(RGBSigmoidPolynomial, c1) == 4,
        "`RGBSigmoidPolynomial::c1` is at byte 4"
    );
    assert!(
        offset_of!(RGBSigmoidPolynomial, c2) == 8,
        "`RGBSigmoidPolynomial::c2` is at byte 8"
    );
};

impl RGBSigmoidPolynomial {
    /// The sigmoid of `c0 λ² + c1 λ + c2`.
    #[must_use]
    pub const fn new(c0: f32, c1: f32, c2: f32) -> Self {
        Self { c0, c1, c2 }
    }

    /// The value at wavelength `lambda` in nm.
    ///
    /// pbrt-v4 `RGBSigmoidPolynomial::operator()`.
    #[must_use]
    pub fn eval(&self, lambda: f32) -> f32 {
        sigmoid(evaluate_polynomial(lambda, [self.c2, self.c1, self.c0]))
    }

    /// The largest value over `[360, 830]` nm: at an end or at the
    /// quadratic's extremum, the sigmoid being monotonic.
    ///
    /// pbrt-v4 `RGBSigmoidPolynomial::MaxValue`.
    #[must_use]
    pub fn max_value(&self) -> f32 {
        let result = max(self.eval(360.0), self.eval(830.0));
        let lambda = -self.c1 / (2.0 * self.c0);
        if (360.0..=830.0).contains(&lambda) {
            max(result, self.eval(lambda))
        } else {
            result
        }
    }
}

/// `½ + x / (2√(1 + x²))`, and its limits 0 and 1 at ∓∞.
///
/// pbrt-v4 `RGBSigmoidPolynomial::s`.
fn sigmoid(x: f32) -> f32 {
    if x.is_infinite() {
        return if x > 0.0 { 1.0 } else { 0.0 };
    }
    0.5 + x / (2.0 * sqrt(1.0 + sqr(x)))
}
