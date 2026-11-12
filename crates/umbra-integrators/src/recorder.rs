//! What a Pixel Trace watches (ADR 0011): the steps of one Pixel Sample,
//! reported by the code that renders it.

use umbra_camera::{CameraRay, CameraSample};
use umbra_spectrum::{SampledSpectrum, SampledWavelengths};

use crate::pixel_sample::FilmSample;

/// The observer of one Pixel Sample's computation. Rendering passes
/// [`NoRecorder`], whose methods do nothing and compile away; a Pixel Trace
/// passes one that records, through the same code (ADR 0011).
///
/// Methods are called in the order of the computation, each at most once
/// per Pixel Sample so far; path vertices and light and BSDF samples join
/// them with the integrators that make them.
pub trait Recorder {
    /// The wavelengths the Pixel Sample carries.
    fn wavelengths(&mut self, lambda: &SampledWavelengths);

    /// The camera sample: film and lens positions, time and filter weight.
    fn camera_sample(&mut self, sample: &CameraSample);

    /// The camera ray, if the camera generated one.
    fn camera_ray(&mut self, ray: &CameraRay);

    /// The radiance arriving along the camera ray, weighted by it.
    fn radiance(&mut self, l: &SampledSpectrum);

    /// What the Film receives: the sensor's RGB and the filter weight.
    fn film_sample(&mut self, sample: &FilmSample);
}

/// The [`Recorder`] of rendering: zero-sized, and records nothing.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct NoRecorder;

impl Recorder for NoRecorder {
    #[inline(always)]
    fn wavelengths(&mut self, _: &SampledWavelengths) {}

    #[inline(always)]
    fn camera_sample(&mut self, _: &CameraSample) {}

    #[inline(always)]
    fn camera_ray(&mut self, _: &CameraRay) {}

    #[inline(always)]
    fn radiance(&mut self, _: &SampledSpectrum) {}

    #[inline(always)]
    fn film_sample(&mut self, _: &FilmSample) {}
}

// Rendering pays nothing for the Recorder seam (ADR 0011).
const _: () = assert!(size_of::<NoRecorder>() == 0, "`NoRecorder` is zero-sized");
