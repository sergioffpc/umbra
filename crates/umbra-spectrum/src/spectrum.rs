//! The `Spectrum` interface, the spectrum types v0.1.0 needs, and
//! `SpectrumKind`, which dispatches between them (ADR 0006).
//!
//! pbrt-v4's `PiecewiseLinearSpectrum` and `BlackbodySpectrum` join with
//! the `.spd` files and blackbody emitters of later milestones (arc42 §4.1).

use umbra_math::max;

use crate::color::{RGB, RGBSigmoidPolynomial, XYZ};
use crate::color_space::RGBColorSpace;
use crate::sampled::{CIE_Y_INTEGRAL, LAMBDA_MAX, LAMBDA_MIN, SampledSpectrum, SampledWavelengths};
use crate::spectra::{self, StdIlluminant};

/// A spectral distribution over wavelengths in nm.
///
/// PBR 4e §4.5, pbrt-v4 `Spectrum`.
pub trait Spectrum {
    /// The value at wavelength `lambda`.
    ///
    /// pbrt-v4 `Spectrum::operator()`.
    fn eval(&self, lambda: f32) -> f32;

    /// A bound on the value over every wavelength.
    ///
    /// pbrt-v4 `Spectrum::MaxValue`.
    fn max_value(&self) -> f32;

    /// The values at the wavelengths `lambda`.
    ///
    /// pbrt-v4 `Spectrum::Sample`.
    fn sample(&self, lambda: &SampledWavelengths) -> SampledSpectrum;
}

/// The spectrum of the value `c` at every wavelength.
///
/// PBR 4e §4.5.1, pbrt-v4 `ConstantSpectrum`.
#[derive(Debug, Clone, Copy, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct ConstantSpectrum {
    c: f32,
}

impl ConstantSpectrum {
    /// `c` at every wavelength.
    #[must_use]
    pub const fn new(c: f32) -> Self {
        Self { c }
    }
}

impl Spectrum for ConstantSpectrum {
    fn eval(&self, _lambda: f32) -> f32 {
        self.c
    }

    fn max_value(&self) -> f32 {
        self.c
    }

    fn sample(&self, _lambda: &SampledWavelengths) -> SampledSpectrum {
        SampledSpectrum::splat(self.c)
    }
}

/// A spectrum tabulated at every integer wavelength from `lambda_min` nm,
/// 0 outside the table. The values are borrowed: the core has no allocator
/// (ADR 0014), so whoever builds the spectrum owns them.
///
/// PBR 4e §4.5.1, pbrt-v4 `DenselySampledSpectrum`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct DenselySampledSpectrum<'a> {
    lambda_min: i32,
    values: &'a [f32],
}

impl<'a> DenselySampledSpectrum<'a> {
    /// The spectrum whose value at `lambda_min + i` nm is `values[i]`.
    ///
    /// # Panics
    ///
    /// If `values` is empty.
    #[must_use]
    pub const fn new(lambda_min: i32, values: &'a [f32]) -> Self {
        assert!(
            !values.is_empty(),
            "a densely sampled spectrum has a sample"
        );
        Self { lambda_min, values }
    }

    /// The first tabulated wavelength.
    #[must_use]
    pub const fn lambda_min(&self) -> i32 {
        self.lambda_min
    }

    /// The values, from [`Self::lambda_min`] at 1 nm.
    #[must_use]
    pub const fn values(&self) -> &'a [f32] {
        self.values
    }

    /// The value at the integer wavelength nearest `lambda`, rounding
    /// halves away from zero as C++ `std::lround`; 0 off the table.
    fn lookup(&self, lambda: f32) -> f32 {
        #[expect(
            clippy::cast_possible_truncation,
            reason = "`std::lround`: a wavelength far off the table saturates and stays off it"
        )]
        let nearest = libm::roundf(lambda) as i64;
        usize::try_from(nearest - i64::from(self.lambda_min))
            .ok()
            .and_then(|offset| self.values.get(offset))
            .copied()
            .unwrap_or(0.0)
    }
}

impl Spectrum for DenselySampledSpectrum<'_> {
    fn eval(&self, lambda: f32) -> f32 {
        debug_assert!(lambda > 0.0, "a wavelength of {lambda} nm");
        self.lookup(lambda)
    }

    /// The largest value, as `std::max_element`: the first of equals, NaNs
    /// skipped unless first.
    fn max_value(&self) -> f32 {
        self.values.iter().copied().reduce(max).unwrap_or(0.0)
    }

    fn sample(&self, lambda: &SampledWavelengths) -> SampledSpectrum {
        lambda.map(|l| self.lookup(l))
    }
}

/// The spectrum of a reflectance or transmittance given as an RGB in
/// `[0, 1]`: its sigmoid polynomial, so bounded by 1.
///
/// PBR 4e §4.6.6, pbrt-v4 `RGBAlbedoSpectrum`.
#[derive(Debug, Clone, Copy, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct RGBAlbedoSpectrum {
    rsp: RGBSigmoidPolynomial,
}

impl RGBAlbedoSpectrum {
    /// The spectrum of `rgb` in `cs`, each component in `[0, 1]`.
    ///
    /// pbrt-v4 `RGBAlbedoSpectrum::RGBAlbedoSpectrum`.
    #[must_use]
    pub fn new(cs: &RGBColorSpace, rgb: RGB) -> Self {
        debug_assert!(
            max(rgb.r, max(rgb.g, rgb.b)) <= 1.0,
            "an albedo of {rgb:?}, above 1"
        );
        debug_assert!(
            umbra_math::min(rgb.r, umbra_math::min(rgb.g, rgb.b)) >= 0.0,
            "an albedo of {rgb:?}, below 0"
        );
        Self {
            rsp: cs.to_rgb_coeffs(rgb),
        }
    }
}

impl Spectrum for RGBAlbedoSpectrum {
    fn eval(&self, lambda: f32) -> f32 {
        self.rsp.eval(lambda)
    }

    fn max_value(&self) -> f32 {
        self.rsp.max_value()
    }

    fn sample(&self, lambda: &SampledWavelengths) -> SampledSpectrum {
        lambda.map(|l| self.rsp.eval(l))
    }
}

/// `rgb` divided by twice its largest component, which is within the
/// table's range, and that scale.
fn scaled_coeffs(cs: &RGBColorSpace, rgb: RGB) -> (f32, RGBSigmoidPolynomial) {
    let m = max(rgb.r, max(rgb.g, rgb.b));
    let scale = 2.0 * m;
    let rgb = if scale == 0.0 {
        RGB::default()
    } else {
        rgb / scale
    };
    (scale, cs.to_rgb_coeffs(rgb))
}

/// The spectrum of a non-negative RGB with no upper bound, such as a
/// scattering coefficient: a scale times the sigmoid polynomial of the RGB
/// scaled into `[0, ½]`.
///
/// PBR 4e §4.6.6, pbrt-v4 `RGBUnboundedSpectrum`.
#[derive(Debug, Clone, Copy, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct RGBUnboundedSpectrum {
    scale: f32,
    rsp: RGBSigmoidPolynomial,
}

impl RGBUnboundedSpectrum {
    /// The spectrum of `rgb` in `cs`, each component non-negative.
    ///
    /// pbrt-v4 `RGBUnboundedSpectrum::RGBUnboundedSpectrum`.
    #[must_use]
    pub fn new(cs: &RGBColorSpace, rgb: RGB) -> Self {
        let (scale, rsp) = scaled_coeffs(cs, rgb);
        Self { scale, rsp }
    }
}

impl Spectrum for RGBUnboundedSpectrum {
    fn eval(&self, lambda: f32) -> f32 {
        self.scale * self.rsp.eval(lambda)
    }

    fn max_value(&self) -> f32 {
        self.scale * self.rsp.max_value()
    }

    fn sample(&self, lambda: &SampledWavelengths) -> SampledSpectrum {
        lambda.map(|l| self.scale * self.rsp.eval(l))
    }
}

/// The spectrum of light given as an RGB: an [`RGBUnboundedSpectrum`]
/// times the colour space's illuminant, so that white is the space's white.
///
/// PBR 4e §4.6.6, pbrt-v4 `RGBIlluminantSpectrum`.
#[derive(Debug, Clone, Copy, PartialEq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub struct RGBIlluminantSpectrum {
    scale: f32,
    rsp: RGBSigmoidPolynomial,
    illuminant: StdIlluminant,
}

impl RGBIlluminantSpectrum {
    /// The spectrum of light of colour `rgb` in `cs`, each component
    /// non-negative.
    ///
    /// pbrt-v4 `RGBIlluminantSpectrum::RGBIlluminantSpectrum`.
    #[must_use]
    pub fn new(cs: &RGBColorSpace, rgb: RGB) -> Self {
        let (scale, rsp) = scaled_coeffs(cs, rgb);
        Self {
            scale,
            rsp,
            illuminant: cs.std_illuminant(),
        }
    }

    /// The colour space's illuminant that the colour multiplies.
    ///
    /// pbrt-v4 `RGBIlluminantSpectrum::Illuminant`.
    #[must_use]
    pub fn illuminant(&self) -> &'static DenselySampledSpectrum<'static> {
        self.illuminant.spectrum()
    }
}

impl Spectrum for RGBIlluminantSpectrum {
    fn eval(&self, lambda: f32) -> f32 {
        self.scale * self.rsp.eval(lambda) * self.illuminant().eval(lambda)
    }

    fn max_value(&self) -> f32 {
        self.scale * self.rsp.max_value() * self.illuminant().max_value()
    }

    fn sample(&self, lambda: &SampledWavelengths) -> SampledSpectrum {
        let s = lambda.map(|l| self.scale * self.rsp.eval(l));
        s * self.illuminant().sample(lambda)
    }
}

/// Every spectrum type, dispatched by `match` (ADR 0006).
///
/// pbrt-v4 `Spectrum`, the `TaggedPointer`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum SpectrumKind<'a> {
    /// A [`ConstantSpectrum`].
    Constant(ConstantSpectrum),
    /// A [`DenselySampledSpectrum`].
    DenselySampled(DenselySampledSpectrum<'a>),
    /// An [`RGBAlbedoSpectrum`].
    RGBAlbedo(RGBAlbedoSpectrum),
    /// An [`RGBUnboundedSpectrum`].
    RGBUnbounded(RGBUnboundedSpectrum),
    /// An [`RGBIlluminantSpectrum`].
    RGBIlluminant(RGBIlluminantSpectrum),
}

impl Spectrum for SpectrumKind<'_> {
    fn eval(&self, lambda: f32) -> f32 {
        match self {
            Self::Constant(s) => s.eval(lambda),
            Self::DenselySampled(s) => s.eval(lambda),
            Self::RGBAlbedo(s) => s.eval(lambda),
            Self::RGBUnbounded(s) => s.eval(lambda),
            Self::RGBIlluminant(s) => s.eval(lambda),
        }
    }

    fn max_value(&self) -> f32 {
        match self {
            Self::Constant(s) => s.max_value(),
            Self::DenselySampled(s) => s.max_value(),
            Self::RGBAlbedo(s) => s.max_value(),
            Self::RGBUnbounded(s) => s.max_value(),
            Self::RGBIlluminant(s) => s.max_value(),
        }
    }

    fn sample(&self, lambda: &SampledWavelengths) -> SampledSpectrum {
        match self {
            Self::Constant(s) => s.sample(lambda),
            Self::DenselySampled(s) => s.sample(lambda),
            Self::RGBAlbedo(s) => s.sample(lambda),
            Self::RGBUnbounded(s) => s.sample(lambda),
            Self::RGBIlluminant(s) => s.sample(lambda),
        }
    }
}

impl From<ConstantSpectrum> for SpectrumKind<'_> {
    fn from(s: ConstantSpectrum) -> Self {
        Self::Constant(s)
    }
}

impl<'a> From<DenselySampledSpectrum<'a>> for SpectrumKind<'a> {
    fn from(s: DenselySampledSpectrum<'a>) -> Self {
        Self::DenselySampled(s)
    }
}

impl From<RGBAlbedoSpectrum> for SpectrumKind<'_> {
    fn from(s: RGBAlbedoSpectrum) -> Self {
        Self::RGBAlbedo(s)
    }
}

impl From<RGBUnboundedSpectrum> for SpectrumKind<'_> {
    fn from(s: RGBUnboundedSpectrum) -> Self {
        Self::RGBUnbounded(s)
    }
}

impl From<RGBIlluminantSpectrum> for SpectrumKind<'_> {
    fn from(s: RGBIlluminantSpectrum) -> Self {
        Self::RGBIlluminant(s)
    }
}

/// `Σ f(λ) g(λ)` over the integer wavelengths of `[LAMBDA_MIN,
/// LAMBDA_MAX]`, summed in order: their inner product at 1 nm.
///
/// PBR 4e §4.5.1, pbrt-v4 `InnerProduct(Spectrum, Spectrum)`.
#[must_use]
pub fn inner_product(f: &(impl Spectrum + ?Sized), g: &(impl Spectrum + ?Sized)) -> f32 {
    let mut integral = 0.0;
    let mut lambda = LAMBDA_MIN;
    while lambda <= LAMBDA_MAX {
        integral += f.eval(lambda) * g.eval(lambda);
        lambda += 1.0;
    }
    integral
}

/// The XYZ of `s`: its inner products with the CIE matching functions,
/// normalised by [`CIE_Y_INTEGRAL`].
///
/// PBR 4e §4.6.1, pbrt-v4 `SpectrumToXYZ`.
#[must_use]
pub fn spectrum_to_xyz(s: &(impl Spectrum + ?Sized)) -> XYZ {
    XYZ::new(
        inner_product(&spectra::X, s),
        inner_product(&spectra::Y, s),
        inner_product(&spectra::Z, s),
    ) / CIE_Y_INTEGRAL
}

/// The photometric power of `s`, its inner product with ȳ; of an
/// [`RGBIlluminantSpectrum`], that of its illuminant alone, the colour
/// being left to the light.
///
/// PBR 4e §4.4.1, pbrt-v4 `SpectrumToPhotometric`.
#[must_use]
pub fn spectrum_to_photometric(s: &SpectrumKind<'_>) -> f32 {
    match s {
        SpectrumKind::RGBIlluminant(s) => inner_product(&spectra::Y, s.illuminant()),
        SpectrumKind::Constant(_)
        | SpectrumKind::DenselySampled(_)
        | SpectrumKind::RGBAlbedo(_)
        | SpectrumKind::RGBUnbounded(_) => inner_product(&spectra::Y, s),
    }
}
