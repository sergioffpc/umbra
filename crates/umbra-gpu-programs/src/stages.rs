//! The wavefront's stages: one OptiX program per stage, one thread per
//! Pixel Sample of the batch, with a structure-of-arrays queue between
//! them (ADR 0026, ADR 0028). On `nvptx64` only.
//!
//! Each stage calls the core's steps on values, as `evaluate_pixel_sample`
//! calls them on the CPU; only the composition is the GPU's own. So far a
//! path is its camera ray: the camera stage takes a Pixel Sample to the
//! radiance along it, the integrator's `li` in one step, and the film stage
//! to the sensor's RGB. The intersection and shading stages that split `li`
//! arrive with the Shapes and lights that need them.

use umbra_camera::{Camera, get_camera_sample};
use umbra_integrators::{NoRecorder, PixelSampleState, RayIntegrator, SceneView};
use umbra_sampling::{HaltonRandomizer, HaltonSampler, Sampler, SamplerKind};
use umbra_spectrum::{SampledSpectrum, SampledWavelengths};

use crate::optix;
use crate::wavefront::{
    FilmSampleRecord, RandomizerRecord, SamplerRecord, SceneRecord, WavefrontParams, WorkUnitRecord,
};

impl SceneRecord {
    /// The core's view of the Scene, its tables in device memory.
    fn view(&self) -> SceneView<'_> {
        SceneView::new(&self.camera, self.filter.view(), self.sensor.view())
    }
}

impl SamplerRecord {
    /// The Render Job's sampler, before any Pixel Sample starts.
    fn sampler(&self) -> SamplerKind<'_> {
        let randomizer = match &self.randomizer {
            RandomizerRecord::None => HaltonRandomizer::None,
            RandomizerRecord::PermuteDigits(permutations) => {
                HaltonRandomizer::PermuteDigits(permutations.view())
            }
            RandomizerRecord::Owen => HaltonRandomizer::Owen,
        };
        HaltonSampler::new(self.samples_per_pixel, self.resolution, randomizer).into()
    }
}

/// The batch's Work Unit that Pixel Sample `i` is one of: the last whose
/// first Pixel Sample is not after it.
///
/// # Panics
///
/// If the first Work Unit does not start at Pixel Sample 0.
fn unit_of(units: &[WorkUnitRecord], i: u32) -> &WorkUnitRecord {
    &units[units.partition_point(|unit| unit.first_pixel_sample <= i) - 1]
}

/// The camera stage: wavelengths, camera sample, camera ray and the
/// radiance along it, for Pixel Sample `i` of the launch index, into the
/// queue.
#[expect(
    unsafe_code,
    reason = "an OptiX entry point keeps its name in the PTX (ADR 0007, ADR 0028)"
)]
#[unsafe(no_mangle)]
extern "ptx-kernel" fn __raygen__camera() {
    let params = wavefront_params();
    let i = optix::launch_index_x();
    let scene = &params.scene.as_ref()[0];
    let view = scene.view();
    let id = unit_of(params.units.as_ref(), i).pixel_sample(i);
    let mut state = PixelSampleState {
        sampler: params.sampler.as_ref()[0].sampler(),
        recorder: NoRecorder,
    };
    state
        .sampler
        .start_pixel_sample(id.pixel, id.sample_index, 0);
    let mut lambda = SampledWavelengths::sample_visible(state.sampler.get_1d());
    let camera_sample = get_camera_sample(&mut state.sampler, id.pixel, view.filter());
    let radiance = match view.camera().generate_ray(camera_sample, &lambda) {
        Some(camera_ray) => {
            camera_ray.weight
                * scene
                    .integrator
                    .li(&view, &camera_ray.ray, &mut lambda, &mut state)
        }
        None => SampledSpectrum::splat(0.0),
    };
    let queue = params.queue;
    let (wavelengths, pdf) = lambda.to_parts();
    queue.lambda.write(i, wavelengths);
    queue.pdf.write(i, pdf.to_array());
    queue.radiance.write(i, radiance.to_array());
    queue.filter_weight.write(i, camera_sample.filter_weight);
}

/// The film stage: the radiance of Pixel Sample `i` of the launch index,
/// from the queue, in the sensor's RGB, with its filter weight.
#[expect(
    unsafe_code,
    reason = "an OptiX entry point keeps its name in the PTX (ADR 0007, ADR 0028)"
)]
#[unsafe(no_mangle)]
extern "ptx-kernel" fn __raygen__film() {
    let params = wavefront_params();
    let i = optix::launch_index_x();
    let view = params.scene.as_ref()[0].view();
    let queue = params.queue;
    let lambda = SampledWavelengths::from_parts(
        queue.lambda.read(i),
        SampledSpectrum::from(queue.pdf.read(i)),
    );
    let rgb = view
        .sensor()
        .to_sensor_rgb(SampledSpectrum::from(queue.radiance.read(i)), &lambda);
    params.film_samples.write(
        i,
        FilmSampleRecord {
            rgb: [rgb.r, rgb.g, rgb.b],
            weight: queue.filter_weight.read(i),
        },
    );
}

/// The launch parameters of a stage, from the `.const` variable `params`.
#[expect(
    unsafe_code,
    reason = "reads OptiX's launch parameters by their address (ADR 0007, ADR 0028)"
)]
fn wavefront_params() -> WavefrontParams {
    let params = optix::launch_params_address() as *const WavefrontParams;
    // SAFETY: the host launches the stages with a `WavefrontParams`, which
    // fits `params` (`programs.rs`); nothing writes it during a launch.
    unsafe { params.read() }
}
