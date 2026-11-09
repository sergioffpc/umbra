//! pbrt-v4 `src/pbrt/util/spectrum_test.cpp`, the cases of the spectra
//! `umbra-spectrum` ports, and `spectrum.h` against pbrt-v4's values.

use umbra_sampling::{Rng, sample_visible_wavelengths, visible_wavelengths_pdf};
use umbra_spectrum::{
    CIE_Y_INTEGRAL, ConstantSpectrum, LAMBDA_MAX, LAMBDA_MIN, RGB, RGBAlbedoSpectrum,
    RGBColorSpace, RGBIlluminantSpectrum, RGBUnboundedSpectrum, SampledSpectrum,
    SampledWavelengths, Spectrum, spectra,
};

use super::known_values::{SAMPLE_UNIFORM, SAMPLE_VISIBLE, SPECTRA, TO_XYZ};
use super::{assert_close, assert_ulps, rgb};
use crate::generators::stratified_1d;

/// The integer wavelengths 360, 361, …, 830 nm.
fn integer_wavelengths() -> impl Iterator<Item = f32> {
    (360_u16..=830).map(f32::from)
}

/// `TEST(Spectrum, XYZ)`.
#[test]
fn xyz() {
    // The integral of each matching function's samples is about
    // `CIE_Y_INTEGRAL`.
    for (cmf, name) in [(&spectra::X, "x"), (&spectra::Y, "y"), (&spectra::Z, "z")] {
        let integral = integer_wavelengths().map(|l| cmf.eval(l)).sum::<f32>() / CIE_Y_INTEGRAL;
        assert!((1.0 - integral).abs() < 0.005, "{name}: {integral}");
    }

    // The XYZ of a constant spectrum is about one.
    let n = 100;
    let mut xyz_sum = [0.0; 3];
    for u in stratified_1d(n) {
        let lambda = SampledWavelengths::sample_uniform(u, 360.0..830.0);
        let xyz = SampledSpectrum::splat(1.0).to_xyz(&lambda);
        for (sum, c) in xyz_sum.iter_mut().zip([xyz.x, xyz.y, xyz.z]) {
            *sum += c;
        }
    }
    for sum in xyz_sum {
        let mean = sum / f32::from(n);
        assert!((1.0 - mean).abs() < 0.035, "{mean}");
    }
}

/// Asserts that `s` is at most its `max_value()` (with pbrt-v4's slack) at
/// wavelengths every 0.92 nm.
#[track_caller]
fn assert_bounded(s: &impl Spectrum, what: &str) {
    let m = s.max_value() * 1.00001;
    let mut lambda = 360.0_f32;
    while lambda < 830.0 {
        assert!(
            s.eval(lambda) <= m,
            "{what}: {} > {m} at {lambda}",
            s.eval(lambda)
        );
        lambda += 0.92;
    }
}

/// `TEST(Spectrum, MaxValue)`, without its `PiecewiseLinearSpectrum` and
/// `BlackbodySpectrum` cases.
#[test]
fn max_value() {
    assert_eq!(ConstantSpectrum::new(2.5).max_value(), 2.5);

    let srgb = RGBColorSpace::srgb();
    let mut rng = Rng::default();
    for _ in 0..20 {
        let r = rng.uniform();
        let g = rng.uniform();
        let rgb = RGB::new(r, g, rng.uniform());
        assert_bounded(&RGBAlbedoSpectrum::new(&srgb, rgb), "RGBAlbedoSpectrum");
        assert_bounded(
            &RGBUnboundedSpectrum::new(&srgb, 10.0 * rgb),
            "RGBUnboundedSpectrum",
        );
        assert_bounded(
            &RGBIlluminantSpectrum::new(&srgb, rgb),
            "RGBIlluminantSpectrum",
        );
    }
}

/// `TEST(Spectrum, SamplingPdfY)`.
#[test]
fn sampling_pdf_y() {
    let n = 1000;
    let ysum: f32 = stratified_1d(n)
        .map(|u| {
            let lambda = sample_visible_wavelengths(u);
            let pdf = visible_wavelengths_pdf(lambda);
            if pdf > 0.0 {
                spectra::Y.eval(lambda) / pdf
            } else {
                0.0
            }
        })
        .sum();
    let yint = ysum / f32::from(n);
    assert!(
        ((yint - CIE_Y_INTEGRAL) / CIE_Y_INTEGRAL).abs() < 1e-3,
        "{yint} vs. {CIE_Y_INTEGRAL}"
    );
}

/// `x̄ + ȳ + z̄` at `lambda`.
fn xyz_sum(lambda: f32) -> f32 {
    spectra::X.eval(lambda) + spectra::Y.eval(lambda) + spectra::Z.eval(lambda)
}

/// `TEST(Spectrum, SamplingPdfXYZ)`.
#[test]
fn sampling_pdf_xyz() {
    let n = 10000;
    let (mut imp_sum, mut unif_sum) = (0.0, 0.0);
    for u in stratified_1d(n) {
        let lambda = umbra_math::lerp(u, LAMBDA_MIN, LAMBDA_MAX);
        unif_sum += xyz_sum(lambda) / (1.0 / (LAMBDA_MAX - LAMBDA_MIN));

        let lambda = sample_visible_wavelengths(u);
        let pdf = visible_wavelengths_pdf(lambda);
        if pdf > 0.0 {
            imp_sum += xyz_sum(lambda) / pdf;
        }
    }
    let (imp_int, unif_int) = (imp_sum / f32::from(n), unif_sum / f32::from(n));
    assert!(
        ((imp_int - unif_int) / unif_int).abs() < 1e-3,
        "{imp_int} vs. {unif_int}"
    );
}

/// The CIE matching functions and D65 equal pbrt-v4's, bit for bit, at
/// every integer wavelength, and are 0 off the table.
#[test]
fn known_values_spectra() {
    let srgb = RGBColorSpace::srgb();
    for (lambda, &expected) in integer_wavelengths().zip(SPECTRA) {
        let ours =
            [&spectra::X, &spectra::Y, &spectra::Z, srgb.illuminant()].map(|s| s.eval(lambda));
        assert_ulps(
            ours,
            expected,
            0,
            &format!("CIE X, Y, Z and D65 at {lambda}"),
        );
    }
    assert_eq!(SPECTRA.len(), 471);
    for lambda in [359.4, 830.6, 1000.0] {
        assert_eq!(spectra::Y.eval(lambda), 0.0, "Y at {lambda}");
    }
}

/// `SampledWavelengths::SampleVisible` and `SampleUniform` give pbrt-v4's
/// wavelengths and PDFs: exactly, but for the `atanh` of the wavelengths and
/// the `cosh` of their PDFs.
#[test]
fn known_values_sampled_wavelengths() {
    for &(u, lambda, pdf) in SAMPLE_VISIBLE {
        let ours = SampledWavelengths::sample_visible(f32::from_bits(u));
        let what = format!("SampleVisible({})", f32::from_bits(u));
        assert_ulps([0, 1, 2, 3].map(|i| ours[i]), lambda, 1, &what);
        // `VisibleWavelengthsPDF`'s 3 ulps, at a wavelength 1 ulp apart.
        assert_ulps(ours.pdf().to_array(), pdf, 4, &what);
    }
    for &(u, lambda, pdf) in SAMPLE_UNIFORM {
        let ours = SampledWavelengths::sample_uniform(f32::from_bits(u), 400.0..700.0);
        let what = format!("SampleUniform({}, 400, 700)", f32::from_bits(u));
        assert_ulps([0, 1, 2, 3].map(|i| ours[i]), lambda, 0, &what);
        assert_ulps(ours.pdf().to_array(), pdf, 0, &what);
    }
}

/// `SampledSpectrum::ToXYZ`, `y` and `ToRGB` of an RGB illuminant give
/// pbrt-v4's values, within the differences of the RGB-to-spectrum table
/// and the wavelength PDFs.
#[test]
fn known_values_to_xyz() {
    let srgb = RGBColorSpace::srgb();
    for &(u, rgb_bits, xyz, y, rgb_out) in TO_XYZ {
        let lambda = SampledWavelengths::sample_visible(f32::from_bits(u));
        let s = RGBIlluminantSpectrum::new(&srgb, rgb(rgb_bits)).sample(&lambda);
        let what = format!(
            "{:?} at SampleVisible({})",
            rgb(rgb_bits),
            f32::from_bits(u)
        );
        let ours = s.to_xyz(&lambda);
        assert_close([ours.x, ours.y, ours.z], xyz, TABLE_TOLERANCE, &what);
        assert_close([s.y(&lambda)], [y], TABLE_TOLERANCE, &what);
        let ours = s.to_rgb(&lambda, &srgb);
        assert_close([ours.r, ours.g, ours.b], rgb_out, TABLE_TOLERANCE, &what);
    }
}

/// The error allowed, relative to the larger of its magnitude and 1, in a
/// value computed through the RGB-to-spectrum table, whose coefficients
/// differ from pbrt-v4's in their last bits where glibc's cube root rounds
/// differently from `libm`'s, or through sampled wavelengths. The largest
/// error at the pinned commit is 1.8e-7.
pub(crate) const TABLE_TOLERANCE: f32 = 1e-6;
