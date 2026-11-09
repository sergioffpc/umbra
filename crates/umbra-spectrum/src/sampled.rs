//! `SampledSpectrum` and `SampledWavelengths`: a spectrum at the
//! [`N_SPECTRUM_SAMPLES`] wavelengths a Pixel Sample carries (ADR 0001).
//!
//! Both live in [`Vec4`] lanes, one wavelength per lane. Every lane-wise
//! operation computes the bits of pbrt-v4's loop over the samples, and the
//! reductions (`Average`, `MinComponentValue`, `MaxComponentValue`) keep its
//! order (ADR 0001). The exceptions are [`SampledSpectrum::exp`] and
//! [`SampledSpectrum::pow`], `umbra-math`'s four-lane kernels, which are
//! within 1 ULP of the correctly rounded result (ADR 0022).

use core::ops::{
    Add, AddAssign, Div, DivAssign, Index, Mul, MulAssign, Neg, Range, Sub, SubAssign,
};

use umbra_math::{Vec4, fast_exp, lerp, safe_sqrt};
use umbra_sampling::{sample_visible_wavelengths, visible_wavelengths_pdf};

use crate::color::{RGB, XYZ};
use crate::color_space::RGBColorSpace;
use crate::spectra;
use crate::spectrum::Spectrum;

/// The number of wavelengths a [`SampledSpectrum`] holds: one [`Vec4`]
/// (ADR 0001).
///
/// pbrt-v4 `NSpectrumSamples`.
pub const N_SPECTRUM_SAMPLES: usize = 4;

/// The shortest wavelength, in nm, that spectra are defined over.
///
/// pbrt-v4 `Lambda_min`.
pub const LAMBDA_MIN: f32 = 360.0;

/// The longest wavelength, in nm, that spectra are defined over.
///
/// pbrt-v4 `Lambda_max`.
pub const LAMBDA_MAX: f32 = 830.0;

/// The integral of the CIE ȳ matching function over `[LAMBDA_MIN,
/// LAMBDA_MAX]` at 1 nm, which normalises the XYZ of a spectrum.
///
/// pbrt-v4 `CIE_Y_integral`, the `double` literal rounded to `float` as C++
/// does.
pub const CIE_Y_INTEGRAL: f32 = crate::narrow(106.856_895);

/// The number of [`N_SPECTRUM_SAMPLES`] as a divisor.
const N_SAMPLES: f32 = 4.0;
const _: () = assert!(N_SPECTRUM_SAMPLES == 4, "N_SAMPLES is N_SPECTRUM_SAMPLES");

/// A spectrum's values at the wavelengths of a [`SampledWavelengths`].
///
/// PBR 4e §4.5.4, pbrt-v4 `SampledSpectrum`.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
#[repr(transparent)]
pub struct SampledSpectrum(Vec4);

impl SampledSpectrum {
    /// `c` at every wavelength.
    ///
    /// pbrt-v4 `SampledSpectrum(Float c)`.
    #[inline]
    #[must_use]
    pub const fn splat(c: f32) -> Self {
        Self(Vec4::splat(c))
    }

    /// The values as an array, one per wavelength.
    #[inline]
    #[must_use]
    pub const fn to_array(self) -> [f32; N_SPECTRUM_SAMPLES] {
        self.0.to_array()
    }

    /// `f` applied to each value.
    #[inline]
    fn map(self, f: impl FnMut(f32) -> f32) -> Self {
        Self::from(self.to_array().map(f))
    }

    /// Whether any value is not zero; NaN counts as not zero.
    ///
    /// pbrt-v4 `SampledSpectrum::operator bool`.
    #[inline]
    #[must_use]
    pub fn is_nonzero(&self) -> bool {
        self.0.cmp_ne(Vec4::ZERO).any()
    }

    /// Whether any value is NaN.
    ///
    /// pbrt-v4 `SampledSpectrum::HasNaNs`.
    #[inline]
    #[must_use]
    pub fn has_nans(&self) -> bool {
        self.0.cmp_ne(self.0).any()
    }

    /// The smallest value, compared in order with C++ `std::min`.
    ///
    /// pbrt-v4 `SampledSpectrum::MinComponentValue`.
    #[inline]
    #[must_use]
    pub fn min_component_value(&self) -> f32 {
        self.0.reduce_min()
    }

    /// The largest value, compared in order with C++ `std::max`.
    ///
    /// pbrt-v4 `SampledSpectrum::MaxComponentValue`.
    #[inline]
    #[must_use]
    pub fn max_component_value(&self) -> f32 {
        self.0.reduce_max()
    }

    /// The mean of the values, summed in order.
    ///
    /// pbrt-v4 `SampledSpectrum::Average`.
    #[inline]
    #[must_use]
    pub fn average(&self) -> f32 {
        self.0.reduce_add() / N_SAMPLES
    }

    /// `self / b` at each wavelength, 0 where `b` is 0.
    ///
    /// pbrt-v4 `SafeDiv(SampledSpectrum, SampledSpectrum)`.
    #[inline]
    #[must_use]
    pub fn safe_div(self, b: Self) -> Self {
        Self(b.0.cmp_ne(Vec4::ZERO).select(self.0 / b.0, Vec4::ZERO))
    }

    /// Each value clamped to `[low, high]`, with pbrt-v4 `Clamp`'s
    /// comparisons.
    ///
    /// pbrt-v4 `Clamp(SampledSpectrum, low, high)`.
    #[inline]
    #[must_use]
    pub fn clamp(self, low: f32, high: f32) -> Self {
        let (low, high) = (Vec4::splat(low), Vec4::splat(high));
        let ret = Self(
            self.0
                .cmp_lt(low)
                .select(low, self.0.cmp_gt(high).select(high, self.0)),
        );
        debug_assert!(!ret.has_nans(), "clamp: {ret:?} has NaNs");
        ret
    }

    /// Each value clamped below at 0, as C++ `std::max(0, v)`.
    ///
    /// pbrt-v4 `ClampZero(SampledSpectrum)`.
    #[inline]
    #[must_use]
    pub fn clamp_zero(self) -> Self {
        let ret = Self(Vec4::ZERO.max(self.0));
        debug_assert!(!ret.has_nans(), "clamp_zero: {ret:?} has NaNs");
        ret
    }

    /// The square root of each value.
    ///
    /// pbrt-v4 `Sqrt(SampledSpectrum)`.
    #[inline]
    #[must_use]
    pub fn sqrt(self) -> Self {
        let ret = Self(self.0.sqrt());
        debug_assert!(!ret.has_nans(), "sqrt: {ret:?} has NaNs");
        ret
    }

    /// The square root of each value clamped below at 0.
    ///
    /// pbrt-v4 `SafeSqrt(SampledSpectrum)`.
    #[inline]
    #[must_use]
    pub fn safe_sqrt(self) -> Self {
        let ret = self.map(safe_sqrt);
        debug_assert!(!ret.has_nans(), "safe_sqrt: {ret:?} has NaNs");
        ret
    }

    /// Each value raised to `e`, within 1 ULP, by [`Vec4::pow`].
    ///
    /// pbrt-v4 `Pow(SampledSpectrum, Float)`.
    #[inline]
    #[must_use]
    pub fn pow(self, e: f32) -> Self {
        Self(self.0.pow(e))
    }

    /// `e` raised to each value, within 1 ULP, by [`Vec4::exp`].
    ///
    /// pbrt-v4 `Exp(SampledSpectrum)`.
    #[inline]
    #[must_use]
    pub fn exp(self) -> Self {
        let ret = Self(self.0.exp());
        debug_assert!(!ret.has_nans(), "exp: {ret:?} has NaNs");
        ret
    }

    /// `e` raised to each value, by [`fast_exp`].
    ///
    /// pbrt-v4 `FastExp(SampledSpectrum)`.
    #[inline]
    #[must_use]
    pub fn fast_exp(self) -> Self {
        let ret = self.map(fast_exp);
        debug_assert!(!ret.has_nans(), "fast_exp: {ret:?} has NaNs");
        ret
    }

    /// `(1 − t) · a + t · b`.
    ///
    /// pbrt-v4 `Lerp(Float, SampledSpectrum, SampledSpectrum)`.
    #[inline]
    #[must_use]
    pub fn lerp(t: f32, a: Self, b: Self) -> Self {
        (1.0 - t) * a + t * b
    }

    /// The bilinear interpolation at `p` of the corners `v`, in the order
    /// `(0, 0)`, `(1, 0)`, `(0, 1)`, `(1, 1)`.
    ///
    /// pbrt-v4 `Bilerp(pstd::array<Float, 2>, span<const SampledSpectrum>)`.
    #[inline]
    #[must_use]
    pub fn bilerp(p: [f32; 2], v: [Self; 4]) -> Self {
        let [x, y] = p;
        (1.0 - x) * (1.0 - y) * v[0] + x * (1.0 - y) * v[1] + (1.0 - x) * y * v[2] + x * y * v[3]
    }

    /// The XYZ of this radiance at `lambda`: the Monte Carlo estimate of
    /// its products with the CIE matching functions, normalised by
    /// [`CIE_Y_INTEGRAL`].
    ///
    /// PBR 4e §4.6.2, pbrt-v4 `SampledSpectrum::ToXYZ`.
    #[must_use]
    pub fn to_xyz(self, lambda: &SampledWavelengths) -> XYZ {
        let pdf = lambda.pdf();
        let estimate = |cmf: SampledSpectrum| (cmf * self).safe_div(pdf).average();
        XYZ::new(
            estimate(spectra::X.sample(lambda)),
            estimate(spectra::Y.sample(lambda)),
            estimate(spectra::Z.sample(lambda)),
        ) / CIE_Y_INTEGRAL
    }

    /// The luminance Y alone of [`Self::to_xyz`].
    ///
    /// pbrt-v4 `SampledSpectrum::y`.
    #[must_use]
    pub fn y(self, lambda: &SampledWavelengths) -> f32 {
        let ys = spectra::Y.sample(lambda);
        (ys * self).safe_div(lambda.pdf()).average() / CIE_Y_INTEGRAL
    }

    /// This radiance at `lambda` in the RGB of `cs`.
    ///
    /// pbrt-v4 `SampledSpectrum::ToRGB`.
    #[must_use]
    pub fn to_rgb(self, lambda: &SampledWavelengths, cs: &RGBColorSpace) -> RGB {
        cs.to_rgb(self.to_xyz(lambda))
    }
}

impl From<[f32; N_SPECTRUM_SAMPLES]> for SampledSpectrum {
    /// pbrt-v4 `SampledSpectrum(pstd::span<const Float>)`.
    #[inline]
    fn from(v: [f32; N_SPECTRUM_SAMPLES]) -> Self {
        Self(Vec4::from(v))
    }
}

impl From<SampledSpectrum> for [f32; N_SPECTRUM_SAMPLES] {
    #[inline]
    fn from(s: SampledSpectrum) -> Self {
        s.to_array()
    }
}

impl Index<usize> for SampledSpectrum {
    type Output = f32;
    /// The value at wavelength `i`.
    #[inline]
    fn index(&self, i: usize) -> &f32 {
        &self.0[i]
    }
}

impl Add for SampledSpectrum {
    type Output = Self;
    #[inline]
    fn add(self, s: Self) -> Self {
        Self(self.0 + s.0)
    }
}

impl AddAssign for SampledSpectrum {
    #[inline]
    fn add_assign(&mut self, s: Self) {
        self.0 += s.0;
    }
}

impl Sub for SampledSpectrum {
    type Output = Self;
    #[inline]
    fn sub(self, s: Self) -> Self {
        Self(self.0 - s.0)
    }
}

impl SubAssign for SampledSpectrum {
    #[inline]
    fn sub_assign(&mut self, s: Self) {
        self.0 -= s.0;
    }
}

impl Sub<SampledSpectrum> for f32 {
    type Output = SampledSpectrum;
    /// `a − s` at each wavelength.
    #[inline]
    fn sub(self, s: SampledSpectrum) -> SampledSpectrum {
        debug_assert!(!self.is_nan(), "{self} - {s:?}");
        SampledSpectrum(Vec4::splat(self) - s.0)
    }
}

impl Mul for SampledSpectrum {
    type Output = Self;
    #[inline]
    fn mul(self, s: Self) -> Self {
        Self(self.0 * s.0)
    }
}

impl MulAssign for SampledSpectrum {
    #[inline]
    fn mul_assign(&mut self, s: Self) {
        self.0 *= s.0;
    }
}

impl Mul<f32> for SampledSpectrum {
    type Output = Self;
    #[inline]
    fn mul(self, a: f32) -> Self {
        debug_assert!(!a.is_nan(), "{self:?} * NaN");
        Self(self.0 * a)
    }
}

impl MulAssign<f32> for SampledSpectrum {
    #[inline]
    fn mul_assign(&mut self, a: f32) {
        debug_assert!(!a.is_nan(), "{self:?} *= NaN");
        self.0 *= a;
    }
}

impl Mul<SampledSpectrum> for f32 {
    type Output = SampledSpectrum;
    #[inline]
    fn mul(self, s: SampledSpectrum) -> SampledSpectrum {
        s * self
    }
}

impl Div for SampledSpectrum {
    type Output = Self;
    #[inline]
    fn div(mut self, s: Self) -> Self {
        self /= s;
        self
    }
}

impl DivAssign for SampledSpectrum {
    #[inline]
    fn div_assign(&mut self, s: Self) {
        debug_assert!(
            s.0.cmp_ne(Vec4::ZERO).all(),
            "{self:?} / {s:?}: a zero divisor"
        );
        self.0 /= s.0;
    }
}

impl Div<f32> for SampledSpectrum {
    type Output = Self;
    #[inline]
    fn div(mut self, a: f32) -> Self {
        self /= a;
        self
    }
}

impl DivAssign<f32> for SampledSpectrum {
    #[inline]
    fn div_assign(&mut self, a: f32) {
        debug_assert!(a != 0.0 && !a.is_nan(), "{self:?} / {a}");
        self.0 /= a;
    }
}

impl Neg for SampledSpectrum {
    type Output = Self;
    #[inline]
    fn neg(self) -> Self {
        Self(-self.0)
    }
}

/// The [`N_SPECTRUM_SAMPLES`] wavelengths, in nm, that a Pixel Sample
/// carries, and the PDF each was sampled with.
///
/// PBR 4e §4.5.4, pbrt-v4 `SampledWavelengths`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SampledWavelengths {
    lambda: Vec4,
    pdf: SampledSpectrum,
}

impl SampledWavelengths {
    /// Wavelengths spaced evenly over `range`, the first at `u` along it
    /// and the others following at a fixed step, wrapping around; each with
    /// the uniform PDF.
    ///
    /// pbrt-v4 `SampledWavelengths::SampleUniform`.
    #[must_use]
    pub fn sample_uniform(u: f32, range: Range<f32>) -> Self {
        let Range {
            start: lambda_min,
            end: lambda_max,
        } = range;
        let mut lambda = [lerp(u, lambda_min, lambda_max); N_SPECTRUM_SAMPLES];
        let delta = (lambda_max - lambda_min) / N_SAMPLES;
        for i in 1..N_SPECTRUM_SAMPLES {
            lambda[i] = wrap(lambda[i - 1] + delta, lambda_min, lambda_max);
        }
        Self {
            lambda: Vec4::from(lambda),
            pdf: SampledSpectrum::splat(1.0 / (lambda_max - lambda_min)),
        }
    }

    /// Wavelengths importance-sampled from the visible-wavelength
    /// distribution, stratified: wavelength `i` from `u + i / 4`, wrapped
    /// to `[0, 1]`.
    ///
    /// PBR 4e §5.4.2, pbrt-v4 `SampledWavelengths::SampleVisible`.
    #[must_use]
    pub fn sample_visible(u: f32) -> Self {
        let lambda = [0.0, 1.0, 2.0, 3.0]
            .map(|i: f32| sample_visible_wavelengths(wrap(u + i / N_SAMPLES, 0.0, 1.0)));
        Self {
            lambda: Vec4::from(lambda),
            pdf: SampledSpectrum::from(lambda.map(visible_wavelengths_pdf)),
        }
    }

    /// `f` of each wavelength, as the spectrum of those values.
    #[inline]
    pub(crate) fn map(&self, f: impl FnMut(f32) -> f32) -> SampledSpectrum {
        SampledSpectrum::from(self.lambda.to_array().map(f))
    }

    /// The PDF of each wavelength.
    ///
    /// pbrt-v4 `SampledWavelengths::PDF`.
    #[inline]
    #[must_use]
    pub fn pdf(&self) -> SampledSpectrum {
        self.pdf
    }

    /// Keeps only the first wavelength, as dispersion does: the others'
    /// PDFs become 0 and the first's is divided by their number. Does
    /// nothing if they are already terminated.
    ///
    /// PBR 4e §4.5.4, pbrt-v4 `SampledWavelengths::TerminateSecondary`.
    pub fn terminate_secondary(&mut self) {
        if self.secondary_terminated() {
            return;
        }
        let mut pdf = [0.0; N_SPECTRUM_SAMPLES];
        pdf[0] = self.pdf[0] / N_SAMPLES;
        self.pdf = SampledSpectrum::from(pdf);
    }

    /// Whether every wavelength but the first has PDF 0.
    ///
    /// pbrt-v4 `SampledWavelengths::SecondaryTerminated`.
    #[must_use]
    pub fn secondary_terminated(&self) -> bool {
        self.pdf.to_array()[1..].iter().all(|&p| p == 0.0)
    }
}

/// `lambda` wrapped back into `[min, max]` once if it is past `max`.
fn wrap(lambda: f32, min: f32, max: f32) -> f32 {
    if lambda > max {
        min + (lambda - max)
    } else {
        lambda
    }
}

impl Index<usize> for SampledWavelengths {
    type Output = f32;
    /// Wavelength `i`, in nm.
    #[inline]
    fn index(&self, i: usize) -> &f32 {
        &self.lambda[i]
    }
}
