//! The pixel sensor (pbrt-v4 `PixelSensor`, `src/pbrt/film.h`,
//! `film.cpp`): how the radiance of a Pixel Sample becomes the sensor's RGB,
//! which the Film accumulates.

use umbra_math::SquareMatrix;
use umbra_spectrum::{
    DenselySampledSpectrum, RGB, SampledSpectrum, SampledWavelengths, Spectrum, spectra,
};

/// One response curve of a sensor: its values at every integer wavelength
/// from `lambda_min`, in storage `S`.
#[derive(Debug, Clone, Copy, PartialEq)]
#[repr(C)]
struct Response<S> {
    values: S,
    lambda_min: i32,
}

impl<S> Response<S> {
    /// The same curve, its storage moved by `f`.
    fn map_storage<T>(self, f: impl FnOnce(S) -> T) -> Response<T> {
        Response {
            values: f(self.values),
            lambda_min: self.lambda_min,
        }
    }
}

impl<S: AsRef<[f32]>> Response<S> {
    /// The curve as the spectrum it tabulates.
    fn spectrum(&self) -> DenselySampledSpectrum<'_> {
        DenselySampledSpectrum::new(self.lambda_min, self.values.as_ref())
    }

    /// The same curve, borrowed.
    fn view(&self) -> Response<&[f32]> {
        Response {
            values: self.values.as_ref(),
            lambda_min: self.lambda_min,
        }
    }
}

impl<'a> From<&DenselySampledSpectrum<'a>> for Response<&'a [f32]> {
    fn from(spectrum: &DenselySampledSpectrum<'a>) -> Self {
        Self {
            values: spectrum.values(),
            lambda_min: spectrum.lambda_min(),
        }
    }
}

/// A sensor's three spectral response curves and the ratio that scales
/// them, and the matrix from its RGB to XYZ.
///
/// The curves' values are in storage `S`: `&'static [f32]` for the CIE 1931
/// matching functions, which are `static`, and device memory on the GPU
/// (ADR 0028).
///
/// PBR 4e §5.4.2, pbrt-v4 `PixelSensor`.
// `repr(C)` for every `S`: the GPU Worker copies it to the device byte for
// byte, with its storage in device memory (ADR 0028, arc42 §8.12).
#[derive(Debug, Clone, PartialEq)]
#[repr(C)]
pub struct PixelSensor<S> {
    /// r̄, ḡ and b̄, in that order.
    responses: [Response<S>; 3],
    xyz_from_sensor_rgb: SquareMatrix<3>,
    imaging_ratio: f32,
}

// The GPU Worker copies the sensor to the device byte for byte, its curves
// in device memory (ADR 0028, arc42 §8.12): its layout is the same on
// `x86_64` and `nvptx64`.
const _: () = {
    use core::mem::offset_of;

    // The storage as the GPU Worker's: a device address and a length, 16
    // bytes aligned to 8 (`umbra-gpu-programs`' `DeviceSlice`).
    type DeviceStorage = [u64; 2];

    assert!(
        size_of::<Response<DeviceStorage>>() == 24,
        "`Response` is 24 bytes, the last 4 padding"
    );
    assert!(
        offset_of!(Response<DeviceStorage>, lambda_min) == 16,
        "`Response::lambda_min` is at byte 16"
    );
    assert!(
        size_of::<PixelSensor<DeviceStorage>>() == 112,
        "`PixelSensor` is three `Response`s, its matrix and its ratio"
    );
    assert!(
        offset_of!(PixelSensor<DeviceStorage>, xyz_from_sensor_rgb) == 72,
        "`PixelSensor::xyz_from_sensor_rgb` is at byte 72"
    );
    assert!(
        offset_of!(PixelSensor<DeviceStorage>, imaging_ratio) == 108,
        "`PixelSensor::imaging_ratio` is at byte 108"
    );
};

impl PixelSensor<&'static [f32]> {
    /// The sensor whose responses are the CIE 1931 matching functions, so
    /// its RGB is XYZ, with no white balancing: pbrt-v4's default `"cie1931"`
    /// sensor. `imaging_ratio` is the exposure time times the ISO over 100,
    /// 1 by default.
    ///
    /// pbrt-v4 `PixelSensor(outputColorSpace, nullptr, imagingRatio)`.
    #[must_use]
    pub fn cie1931(imaging_ratio: f32) -> Self {
        Self {
            responses: [&spectra::X, &spectra::Y, &spectra::Z].map(Response::from),
            xyz_from_sensor_rgb: SquareMatrix::default(),
            imaging_ratio,
        }
    }
}

impl<S> PixelSensor<S> {
    /// The same sensor, its curves' storage moved by `f`: to device memory,
    /// for the GPU Worker (ADR 0028).
    #[must_use]
    pub fn map_storage<T>(self, mut f: impl FnMut(S) -> T) -> PixelSensor<T> {
        PixelSensor {
            responses: self.responses.map(|response| response.map_storage(&mut f)),
            xyz_from_sensor_rgb: self.xyz_from_sensor_rgb,
            imaging_ratio: self.imaging_ratio,
        }
    }

    /// The matrix from the sensor's RGB to XYZ.
    ///
    /// pbrt-v4 `PixelSensor::XYZFromSensorRGB`.
    #[must_use]
    pub fn xyz_from_sensor_rgb(&self) -> &SquareMatrix<3> {
        &self.xyz_from_sensor_rgb
    }
}

impl<S: AsRef<[f32]>> PixelSensor<S> {
    /// The sensor's RGB for the radiance `l` at the wavelengths `lambda`:
    /// each response's Monte Carlo estimate over the wavelengths.
    ///
    /// pbrt-v4 `PixelSensor::ToSensorRGB`.
    #[must_use]
    pub fn to_sensor_rgb(&self, l: SampledSpectrum, lambda: &SampledWavelengths) -> RGB {
        let l = l.safe_div(lambda.pdf());
        let [r, g, b] = self.responses.each_ref().map(|response| {
            self.imaging_ratio * (response.spectrum().sample(lambda) * l).average()
        });
        RGB::new(r, g, b)
    }

    /// The same sensor, borrowed: what per-sample code reads.
    #[must_use]
    pub fn view(&self) -> PixelSensor<&[f32]> {
        PixelSensor {
            responses: self.responses.each_ref().map(Response::view),
            xyz_from_sensor_rgb: self.xyz_from_sensor_rgb,
            imaging_ratio: self.imaging_ratio,
        }
    }
}
