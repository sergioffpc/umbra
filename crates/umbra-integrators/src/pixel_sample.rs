//! One Pixel Sample from start to end (pbrt-v4
//! `RayIntegrator::EvaluatePixelSample`, `src/pbrt/cpu/integrators.cpp`).

use umbra_camera::{Camera, get_camera_sample};
use umbra_geometry::Ray;
use umbra_math::Point2i;
use umbra_sampling::{Sampler, SamplerKind};
use umbra_spectrum::{RGB, SampledSpectrum, SampledWavelengths};

use crate::recorder::Recorder;
use crate::view::SceneView;

/// Which Pixel Sample: a pixel and a sample index, the pair that identifies
/// it within a Render Job.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct PixelSampleId {
    /// The pixel, in raster coordinates.
    pub pixel: Point2i,
    /// The sample index within the pixel.
    pub sample_index: u32,
}

/// What one Pixel Sample adds to its pixel of the Film: the sensor's RGB
/// and the filter weight, which the Film sums as `weight · rgb` and
/// `weight`.
///
/// pbrt-v4 `RGBFilm::AddSample`'s arguments, after `ToSensorRGB`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FilmSample {
    /// The radiance in the sensor's RGB.
    pub rgb: RGB,
    /// The filter weight of the camera sample.
    pub weight: f32,
}

/// What one Pixel Sample changes as it is computed: the sampler it draws
/// from and the recorder that watches it. A thread keeps one and reuses it
/// for every Pixel Sample it computes, so nothing is allocated per Pixel
/// Sample (ADR 0008).
#[derive(Debug, Clone)]
pub struct PixelSampleState<'s, R> {
    /// The sampler, restarted at each Pixel Sample.
    pub sampler: SamplerKind<'s>,
    /// The recorder: `NoRecorder` when rendering.
    pub recorder: R,
}

/// An integrator that estimates the radiance arriving along a ray: the
/// per-ray part of [`evaluate_pixel_sample`].
///
/// PBR 4e §1.3.4, pbrt-v4 `RayIntegrator`.
pub trait RayIntegrator {
    /// The radiance arriving at the ray's origin along it, at the
    /// wavelengths `lambda`, drawing what it needs from `state`'s sampler.
    ///
    /// pbrt-v4 `RayIntegrator::Li`.
    fn li<R: Recorder>(
        &self,
        view: &SceneView<'_>,
        ray: &Ray,
        lambda: &mut SampledWavelengths,
        state: &mut PixelSampleState<'_, R>,
    ) -> SampledSpectrum;
}

/// The Pixel Sample `id`: wavelengths, camera sample, camera ray and the
/// radiance along it, converted to the sensor's RGB. A pure function of the
/// Scene, the sampler's seed and `id` (ADR 0022), and the one code path that
/// rendering and Pixel Traces share (ADR 0011).
///
/// PBR 4e §1.3.4, pbrt-v4 `RayIntegrator::EvaluatePixelSample` after
/// `Sampler::StartPixelSample`.
pub fn evaluate_pixel_sample<I: RayIntegrator, R: Recorder>(
    integrator: &I,
    view: &SceneView<'_>,
    id: PixelSampleId,
    state: &mut PixelSampleState<'_, R>,
) -> FilmSample {
    state
        .sampler
        .start_pixel_sample(id.pixel, id.sample_index, 0);
    let mut lambda = SampledWavelengths::sample_visible(state.sampler.get_1d());
    state.recorder.wavelengths(&lambda);

    let camera_sample = get_camera_sample(&mut state.sampler, id.pixel, view.filter());
    state.recorder.camera_sample(&camera_sample);

    let l = match view.camera().generate_ray(camera_sample, &lambda) {
        Some(camera_ray) => {
            state.recorder.camera_ray(&camera_ray);
            camera_ray.weight * integrator.li(view, &camera_ray.ray, &mut lambda, state)
        }
        None => SampledSpectrum::splat(0.0),
    };
    state.recorder.radiance(&l);

    let sample = FilmSample {
        rgb: view.sensor().to_sensor_rgb(l, &lambda),
        weight: camera_sample.filter_weight,
    };
    state.recorder.film_sample(&sample);
    sample
}
