//! Moving a filter's or a sensor's tables to other storage, as the GPU
//! Worker moves them into device memory (ADR 0028): the moved value
//! computes what the original does.

use umbra_camera::{Filter, FilterKind, GaussianFilter, PixelSensor};
use umbra_math::{Point2f, Vector2f};
use umbra_spectrum::{SampledSpectrum, SampledWavelengths};

/// The points of `[0, 1)²` on a 16 × 16 grid.
fn grid() -> impl Iterator<Item = Point2f> {
    (0..16u8).flat_map(|i| (0..16u8).map(move |j| Point2f::new(f32::from(i), f32::from(j)) / 16.0))
}

#[test]
fn a_filter_moved_to_other_storage_samples_as_before() {
    let filter: FilterKind<Vec<f32>> =
        GaussianFilter::new_in(Vector2f::new(1.5, 0.75), 0.5, |n| vec![0.0; n]).into();
    let moved: FilterKind<Box<[f32]>> = filter.view().map_storage(Box::from);
    for u in grid() {
        assert_eq!(moved.sample(u), filter.sample(u), "at {u:?}");
    }
}

#[test]
fn a_sensor_moved_to_other_storage_converts_radiance_as_before() {
    let sensor = PixelSensor::cie1931(0.5);
    let moved: PixelSensor<Box<[f32]>> = sensor.view().map_storage(Box::from);
    let l = SampledSpectrum::from([0.25, 1.0, 2.0, 4.0]);
    for u in grid() {
        let lambda = SampledWavelengths::sample_visible(u.x);
        assert_eq!(
            moved.to_sensor_rgb(l, &lambda),
            sensor.to_sensor_rgb(l, &lambda),
            "at u {}",
            u.x
        );
    }
}
