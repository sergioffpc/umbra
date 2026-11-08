//! pbrt-v4 `src/pbrt/util/color_test.cpp`, the sRGB cases, and `color.h`
//! and `colorspace.h` against pbrt-v4's values.

use umbra_spectrum::{
    DenselySampledSpectrum, RGB, RGBAlbedoSpectrum, RGBColorSpace, RGBIlluminantSpectrum,
    RGBUnboundedSpectrum, Spectrum, SpectrumKind, XYZ, spectrum_to_photometric, spectrum_to_xyz,
};

use super::known_values::{
    LAMBDAS, RGB_ALBEDO, RGB_ILLUMINANT, RGB_UNBOUNDED, SRGB_CHROMATICITIES, SRGB_RGB_FROM_XYZ,
    SRGB_WHITE_XYZ, SRGB_XYZ_FROM_RGB,
};
use super::spectrum_test::TABLE_TOLERANCE;
use super::{assert_close, assert_ulps, rgb};
use crate::generators::colors;

/// Asserts that `a` and `b` are within `eps` component-wise.
#[track_caller]
fn assert_rgb_near(a: RGB, b: RGB, eps: f32, what: &str) {
    let close = [a.r - b.r, a.g - b.g, a.b - b.b]
        .iter()
        .all(|d| d.abs() < eps);
    assert!(close, "{what}: {a:?} vs {b:?}");
}

/// `TEST(RGBColorSpace, RGBXYZ)`, for sRGB.
#[test]
fn rgb_color_space_rgb_xyz() {
    let cs = RGBColorSpace::srgb();
    let xyz = cs.to_xyz(RGB::new(1.0, 1.0, 1.0));
    assert_rgb_near(cs.to_rgb(xyz), RGB::new(1.0, 1.0, 1.0), 1e-4, "white");
}

/// `TEST(RGBColorSpace, sRGB)`: the columns of the XYZ-to-RGB matrix.
#[test]
fn rgb_color_space_srgb() {
    let srgb = RGBColorSpace::srgb();
    for (xyz, expected) in [
        (XYZ::new(1.0, 0.0, 0.0), RGB::new(3.2406, -0.9689, 0.0557)),
        (XYZ::new(0.0, 1.0, 0.0), RGB::new(-1.5372, 1.8758, -0.2040)),
        (XYZ::new(0.0, 0.0, 1.0), RGB::new(-0.4986, 0.0415, 1.0570)),
    ] {
        assert_rgb_near(srgb.to_rgb(xyz), expected, 1e-3, &format!("{xyz:?}"));
    }
}

/// `TEST(RGBColorSpace, StdIllumWhitesRGB)`.
#[test]
fn rgb_color_space_std_illum_white_srgb() {
    let srgb = RGBColorSpace::srgb();
    let rgb = srgb.to_rgb(spectrum_to_xyz(srgb.illuminant()));
    for c in [rgb.r, rgb.g, rgb.b] {
        assert!((0.99..=1.01).contains(&c), "{rgb:?}");
    }
}

/// Asserts that `s` attains its `max_value()` to within 1e-4, sampled
/// every 1/16 nm.
#[track_caller]
fn assert_max_attained(s: &impl Spectrum, what: &str) {
    let m = s.max_value();
    let sampled = (0_u16..=470 * 16)
        .map(|i| s.eval(360.0 + f32::from(i) / 16.0))
        .fold(0.0, f32::max);
    assert!(
        ((sampled - m) / sampled).abs() < 1e-4,
        "{what}: sampled {sampled} MaxValue {m}"
    );
}

/// `TEST(RGBUnboundedSpectrum, MaxValue)`, for sRGB.
#[test]
fn rgb_unbounded_spectrum_max_value() {
    let srgb = RGBColorSpace::srgb();
    for rgb in colors(100, 1.0, 0.0) {
        let rgb = rgb * 10.0;
        assert_max_attained(&RGBUnboundedSpectrum::new(&srgb, rgb), &format!("{rgb:?}"));
    }
}

/// `TEST(RGBAlbedoSpectrum, MaxValue)`, for sRGB.
#[test]
fn rgb_albedo_spectrum_max_value() {
    let srgb = RGBColorSpace::srgb();
    for rgb in colors(100, 1.0, 0.0) {
        assert_max_attained(&RGBAlbedoSpectrum::new(&srgb, rgb), &format!("{rgb:?}"));
    }
}

/// The RGB in `cs` of `f` densely sampled at 1 nm over the visible range:
/// `cs.ToRGB(SpectrumToXYZ(DenselySampledSpectrum::SampleFunction(f)))`.
fn round_trip(cs: &RGBColorSpace, f: impl Fn(f32) -> f32) -> RGB {
    let values: Vec<f32> = (360_u16..=830).map(|l| f(f32::from(l))).collect();
    cs.to_rgb(spectrum_to_xyz(&DenselySampledSpectrum::new(360, &values)))
}

/// `TEST(RGBAlbedoSpectrum, RoundTripsRGB)`. Some error comes from the
/// optimisation using the CIE curves at 5 nm and this at 1 nm.
#[test]
fn rgb_albedo_spectrum_round_trip_srgb() {
    let cs = RGBColorSpace::srgb();
    for rgb in colors(100, 1.0, 0.0) {
        let rs = RGBAlbedoSpectrum::new(&cs, rgb);
        let rgb2 = round_trip(&cs, |l| rs.eval(l) * cs.illuminant().eval(l));
        assert_rgb_near(rgb, rgb2, 0.01, "round trip");
    }
}

/// `TEST(RGBIlluminantSpectrum, RoundTripsRGB)`.
#[test]
fn rgb_illuminant_spectrum_round_trip_srgb() {
    let cs = RGBColorSpace::srgb();
    for rgb in colors(100, 1.0, 0.0) {
        let rs = RGBIlluminantSpectrum::new(&cs, rgb);
        let rgb2 = round_trip(&cs, |l| rs.eval(l));
        assert_rgb_near(rgb, rgb2, 0.01, "round trip");
    }
}

/// The sRGB colour space has pbrt-v4's white point and matrices, bit for
/// bit, and its white pbrt-v4's XYZ and photometric power.
#[test]
fn known_values_srgb() {
    let srgb = RGBColorSpace::srgb();
    for (p, &expected) in [srgb.r(), srgb.g(), srgb.b(), srgb.w()]
        .iter()
        .zip(SRGB_CHROMATICITIES)
    {
        assert_ulps([p.x, p.y], expected, 0, "sRGB chromaticity");
    }
    for (row, &expected) in srgb.xyz_from_rgb().rows().iter().zip(SRGB_XYZ_FROM_RGB) {
        assert_ulps(*row, expected, 0, "XYZFromRGB");
    }
    for (row, &expected) in srgb.rgb_from_xyz().rows().iter().zip(SRGB_RGB_FROM_XYZ) {
        assert_ulps(*row, expected, 0, "RGBFromXYZ");
    }

    let white = spectrum_to_xyz(srgb.illuminant());
    let photometric = spectrum_to_photometric(&SpectrumKind::from(*srgb.illuminant()));
    let expected = <[u32; 4]>::try_from(SRGB_WHITE_XYZ).unwrap();
    assert_ulps([white.x, white.y, white.z, photometric], expected, 0, "D65");
}

/// Asserts that `s` has pbrt-v4's values at [`LAMBDAS`] and its
/// `max_value`: exactly for a grey, which bypasses the table, and within
/// [`TABLE_TOLERANCE`] otherwise.
#[track_caller]
fn assert_known(s: &impl Spectrum, rgb: RGB, values: [u32; 10], max: u32, what: &str) {
    let ours = LAMBDAS.map(|l| s.eval(f32::from_bits(l)));
    let what = format!("{what}({rgb:?})");
    #[expect(clippy::float_cmp, reason = "the table's exact test for a grey")]
    let grey = rgb.r == rgb.g && rgb.g == rgb.b;
    if grey {
        assert_ulps(ours, values, 0, &what);
        assert_ulps([s.max_value()], [max], 0, &what);
    } else {
        assert_close(ours, values, TABLE_TOLERANCE, &what);
        assert_close([s.max_value()], [max], TABLE_TOLERANCE, &what);
    }
}

/// The RGB spectra give pbrt-v4's values.
#[test]
fn known_values_rgb_spectra() {
    let srgb = RGBColorSpace::srgb();
    for &(bits, values, max) in RGB_ALBEDO {
        let s = RGBAlbedoSpectrum::new(&srgb, rgb(bits));
        assert_known(&s, rgb(bits), values, max, "RGBAlbedoSpectrum");
    }
    for &(bits, values, max) in RGB_UNBOUNDED {
        let s = RGBUnboundedSpectrum::new(&srgb, rgb(bits));
        assert_known(&s, rgb(bits), values, max, "RGBUnboundedSpectrum");
    }
    for &(bits, values, max) in RGB_ILLUMINANT {
        let s = RGBIlluminantSpectrum::new(&srgb, rgb(bits));
        assert_known(&s, rgb(bits), values, max, "RGBIlluminantSpectrum");
    }
}
