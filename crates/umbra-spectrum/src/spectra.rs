//! pbrt-v4's named spectral data as `static`s, computed at compile time
//! (arc42 §8.1): the CIE 1931 matching functions and the illuminants the
//! colour spaces need.
//!
//! pbrt-v4 builds these in `Spectra::Init` from piecewise-linear data. Each
//! is a [`DenselySampledSpectrum`] at 1 nm over `[LAMBDA_MIN, LAMBDA_MAX]`,
//! evaluated here by `const fn` with the same `f32` operations, so the
//! values are pbrt-v4's bits.
//!
//! pbrt-v4 `Spectra::X`, `Spectra::Y`, `Spectra::Z` and `GetNamedSpectrum`.

mod data;

use data::{CIE_ILLUM_D6500, CIE_SAMPLES};

use crate::narrow_all;
use crate::sampled::{CIE_Y_INTEGRAL, LAMBDA_MAX, LAMBDA_MIN};
use crate::spectrum::DenselySampledSpectrum;

/// [`LAMBDA_MIN`] as the first wavelength of a table.
#[expect(clippy::cast_possible_truncation, reason = "360 nm")]
const FIRST: i32 = LAMBDA_MIN as i32;
#[expect(clippy::float_cmp_const, reason = "whole numbers of nm, exact in f32")]
const _: () = assert!(
    LAMBDA_MAX - LAMBDA_MIN + 1.0 == 471.0 && CIE_SAMPLES == 471,
    "the CIE tables cover [LAMBDA_MIN, LAMBDA_MAX] at 1 nm"
);

static X_VALUES: [f32; CIE_SAMPLES] = narrow_all(data::CIE_X);
static Y_VALUES: [f32; CIE_SAMPLES] = narrow_all(data::CIE_Y);
static Z_VALUES: [f32; CIE_SAMPLES] = narrow_all(data::CIE_Z);
static D65_VALUES: [f32; CIE_SAMPLES] =
    densely_sample_normalized(&narrow_all(CIE_ILLUM_D6500), &Y_VALUES);

/// The CIE 1931 x̄ matching function.
///
/// pbrt-v4 `Spectra::X()`.
pub static X: DenselySampledSpectrum<'static> = DenselySampledSpectrum::new(FIRST, &X_VALUES);

/// The CIE 1931 ȳ matching function, the luminous efficiency.
///
/// pbrt-v4 `Spectra::Y()`.
pub static Y: DenselySampledSpectrum<'static> = DenselySampledSpectrum::new(FIRST, &Y_VALUES);

/// The CIE 1931 z̄ matching function.
///
/// pbrt-v4 `Spectra::Z()`.
pub static Z: DenselySampledSpectrum<'static> = DenselySampledSpectrum::new(FIRST, &Z_VALUES);

/// The CIE standard illuminant D65, normalised to luminance 1: the white
/// of sRGB.
///
/// pbrt-v4 `GetNamedSpectrum("stdillum-D65")`, densely sampled as
/// `RGBColorSpace` stores it.
pub static STD_ILLUM_D65: DenselySampledSpectrum<'static> =
    DenselySampledSpectrum::new(FIRST, &D65_VALUES);

/// A standard illuminant, the white of a colour space: a `Copy` name for
/// its `static` spectrum, so that what holds it holds no reference
/// (ADR 0014).
///
/// pbrt-v4 names these `"stdillum-…"` in `GetNamedSpectrum`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
pub enum StdIlluminant {
    /// CIE D65, [`STD_ILLUM_D65`].
    D65,
}

impl StdIlluminant {
    /// The illuminant's spectrum.
    #[must_use]
    pub fn spectrum(self) -> &'static DenselySampledSpectrum<'static> {
        match self {
            Self::D65 => &STD_ILLUM_D65,
        }
    }
}

/// pbrt-v4 `Lerp`: `(1 − t) · a + t · b`.
const fn lerp(t: f32, a: f32, b: f32) -> f32 {
    (1.0 - t) * a + t * b
}

/// The piecewise-linear spectrum of the interleaved `(λ, value)` pairs
/// `samples`, each value times `scale`, at `lambda` (pbrt-v4
/// `PiecewiseLinearSpectrum::operator()` after `Scale(scale)`).
const fn piecewise_linear(samples: &[f32], scale: f32, lambda: f32) -> f32 {
    let n = samples.len() / 2;
    if lambda < samples[0] || lambda > samples[2 * (n - 1)] {
        return 0.0;
    }
    let o = find_interval(samples, lambda);
    let (l0, l1) = (samples[2 * o], samples[2 * o + 2]);
    let (v0, v1) = (samples[2 * o + 1] * scale, samples[2 * o + 3] * scale);
    lerp((lambda - l0) / (l1 - l0), v0, v1)
}

/// The index of the last pair of `samples` whose λ is at most `lambda`,
/// clamped to leave one after it: pbrt-v4 `FindInterval`'s bisection.
const fn find_interval(samples: &[f32], lambda: f32) -> usize {
    let n = samples.len() / 2;
    let mut size = n - 2;
    let mut first = 1;
    while size > 0 {
        let half = size >> 1;
        let middle = first + half;
        if samples[2 * middle] <= lambda {
            first = middle + 1;
            size -= half + 1;
        } else {
            size = half;
        }
    }
    let i = first - 1;
    if i > n - 2 { n - 2 } else { i }
}

/// The interleaved `(λ, value)` pairs `samples`, which cover `[LAMBDA_MIN,
/// LAMBDA_MAX]`, scaled to luminance 1 and densely sampled: pbrt-v4
/// `PiecewiseLinearSpectrum::FromInterleaved(samples, true)`, then
/// `DenselySampledSpectrum` of it. `y` is the dense ȳ the scale is
/// computed against.
const fn densely_sample_normalized<const M: usize>(
    samples: &[f32; M],
    y: &[f32; CIE_SAMPLES],
) -> [f32; CIE_SAMPLES] {
    // Without this, pbrt-v4 extends the samples with constant ends.
    assert!(
        M.is_multiple_of(2) && samples[0] <= LAMBDA_MIN && samples[M - 2] >= LAMBDA_MAX,
        "interleaved samples that cover [LAMBDA_MIN, LAMBDA_MAX]"
    );

    // pbrt-v4 `InnerProduct(spec, &Spectra::Y())`.
    let mut integral = 0.0;
    let (mut i, mut lambda) = (0, LAMBDA_MIN);
    while i < CIE_SAMPLES {
        integral += piecewise_linear(samples, 1.0, lambda) * y[i];
        (i, lambda) = (i + 1, lambda + 1.0);
    }
    let scale = CIE_Y_INTEGRAL / integral;

    let mut dense = [0.0; CIE_SAMPLES];
    let (mut i, mut lambda) = (0, LAMBDA_MIN);
    while i < CIE_SAMPLES {
        dense[i] = piecewise_linear(samples, scale, lambda);
        (i, lambda) = (i + 1, lambda + 1.0);
    }
    dense
}
