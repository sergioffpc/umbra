//! The `known_values` tests of `PixelSensor` against `known_values.rs`.
//! pbrt-v4 has no sensor tests to port.

use umbra_camera::PixelSensor;
use umbra_spectrum::{SampledSpectrum, SampledWavelengths};

use super::known_values::SENSOR_RGB;
use super::{assert_ulps, floats};

#[test]
fn known_values_default_sensor() {
    let sensor = PixelSensor::cie1931(1.0);
    for &(u, l, rgb) in SENSOR_RGB {
        let lambda = SampledWavelengths::sample_visible(f32::from_bits(u));
        let actual = sensor.to_sensor_rgb(SampledSpectrum::from(floats(l)), &lambda);
        assert_ulps(
            [actual.r, actual.g, actual.b],
            rgb,
            8,
            &format!("ToSensorRGB at u {u:#x}"),
        );
    }
}
