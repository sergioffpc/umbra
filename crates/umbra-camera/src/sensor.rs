//! The pixel sensor (pbrt-v4 `PixelSensor`, `src/pbrt/film.h`,
//! `film.cpp`): how the radiance of a Pixel Sample becomes the sensor's RGB,
//! which the Film accumulates.

use umbra_math::SquareMatrix;
use umbra_spectrum::{
    DenselySampledSpectrum, RGB, SampledSpectrum, SampledWavelengths, Spectrum, spectra,
};

/// A sensor's three spectral response curves and the ratio that scales
/// them, and the matrix from its RGB to XYZ.
///
/// The curves are borrowed: the CIE 1931 matching functions are `static`,
/// and the named camera sensors will be the Scene's.
///
/// PBR 4e §5.4.2, pbrt-v4 `PixelSensor`.
#[derive(Debug, Clone, PartialEq)]
pub struct PixelSensor<'a> {
    r_bar: DenselySampledSpectrum<'a>,
    g_bar: DenselySampledSpectrum<'a>,
    b_bar: DenselySampledSpectrum<'a>,
    imaging_ratio: f32,
    xyz_from_sensor_rgb: SquareMatrix<3>,
}

impl PixelSensor<'static> {
    /// The sensor whose responses are the CIE 1931 matching functions, so
    /// its RGB is XYZ, with no white balancing: pbrt-v4's default `"cie1931"`
    /// sensor. `imaging_ratio` is the exposure time times the ISO over 100,
    /// 1 by default.
    ///
    /// pbrt-v4 `PixelSensor(outputColorSpace, nullptr, imagingRatio)`.
    #[must_use]
    pub fn cie1931(imaging_ratio: f32) -> Self {
        Self {
            r_bar: spectra::X,
            g_bar: spectra::Y,
            b_bar: spectra::Z,
            imaging_ratio,
            xyz_from_sensor_rgb: SquareMatrix::default(),
        }
    }
}

impl PixelSensor<'_> {
    /// The sensor's RGB for the radiance `l` at the wavelengths `lambda`:
    /// each response's Monte Carlo estimate over the wavelengths.
    ///
    /// pbrt-v4 `PixelSensor::ToSensorRGB`.
    #[must_use]
    pub fn to_sensor_rgb(&self, l: SampledSpectrum, lambda: &SampledWavelengths) -> RGB {
        let l = l.safe_div(lambda.pdf());
        let response = |bar: &DenselySampledSpectrum<'_>| (bar.sample(lambda) * l).average();
        RGB::new(
            self.imaging_ratio * response(&self.r_bar),
            self.imaging_ratio * response(&self.g_bar),
            self.imaging_ratio * response(&self.b_bar),
        )
    }

    /// The matrix from the sensor's RGB to XYZ.
    ///
    /// pbrt-v4 `PixelSensor::XYZFromSensorRGB`.
    #[must_use]
    pub fn xyz_from_sensor_rgb(&self) -> &SquareMatrix<3> {
        &self.xyz_from_sensor_rgb
    }
}
