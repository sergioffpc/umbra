#![doc = include_str!("../README.md")]
#![no_std]
// No `f64` in the core (ADR 0028): `clippy.toml` lists it.
#![warn(clippy::disallowed_types)]

mod color;
mod color_space;
mod rgb_to_spectrum;
mod sampled;
pub mod spectra;
mod spectrum;

pub use color::{RGB, RGBSigmoidPolynomial, XYZ};
pub use color_space::RGBColorSpace;
pub use sampled::{
    CIE_Y_INTEGRAL, LAMBDA_MAX, LAMBDA_MIN, N_SPECTRUM_SAMPLES, SampledSpectrum, SampledWavelengths,
};
pub use spectrum::{
    ConstantSpectrum, DenselySampledSpectrum, RGBAlbedoSpectrum, RGBIlluminantSpectrum,
    RGBUnboundedSpectrum, Spectrum, SpectrumKind, inner_product, spectrum_to_photometric,
    spectrum_to_xyz,
};

/// `values` rounded to `f32`, as C++ rounds the `double` literals of
/// pbrt-v4's `float` tables and constants.
#[expect(
    clippy::disallowed_types,
    reason = "const evaluation only: no f64 at run time (ADR 0028)"
)]
const fn narrow_all<const N: usize>(values: [f64; N]) -> [f32; N] {
    let mut out = [0.0; N];
    let mut i = 0;
    while i < N {
        out[i] = narrow(values[i]);
        i += 1;
    }
    out
}

/// `v` rounded to the nearest `f32`, ties to even: C++'s conversion of a
/// `double` literal to `float`.
#[expect(
    clippy::cast_possible_truncation,
    clippy::disallowed_types,
    reason = "the rounding C++ applies to pbrt-v4's literals, in const evaluation only (ADR 0028)"
)]
const fn narrow(v: f64) -> f32 {
    v as f32
}
