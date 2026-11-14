//! The volumetric path tracer (pbrt-v4 `VolPathIntegrator`,
//! `src/pbrt/cpu/integrators.h`, `integrators.cpp`).

use umbra_geometry::Ray;
use umbra_spectrum::{SampledSpectrum, SampledWavelengths};

use crate::pixel_sample::{PixelSampleState, RayIntegrator};
use crate::recorder::Recorder;
use crate::view::SceneView;

/// The volumetric path tracer, without media: v0.1.0 refuses Scenes that
/// have them.
///
/// So far the Scene has no Shapes and no lights: every camera ray escapes,
/// and an escaping ray gathers only the infinite lights' emission, so the
/// radiance is zero. Intersections, emission and shading arrive with
/// [#79](https://github.com/sergioffpc/umbra/issues/79) and
/// [#80](https://github.com/sergioffpc/umbra/issues/80).
///
/// PBR 4e §14.2, pbrt-v4 `VolPathIntegrator`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
#[repr(C)]
pub struct VolPathIntegrator {
    max_depth: u32,
}

// The GPU Worker copies the integrator to the device byte for byte
// (ADR 0028, arc42 §8.12): its layout is the same on `x86_64` and `nvptx64`.
const _: () = assert!(
    size_of::<VolPathIntegrator>() == 4,
    "`VolPathIntegrator` is its `u32` depth"
);

impl VolPathIntegrator {
    /// The integrator that follows paths of up to `max_depth` bounces;
    /// pbrt-v4's default is 5.
    #[must_use]
    pub const fn new(max_depth: u32) -> Self {
        Self { max_depth }
    }

    /// The largest number of bounces a path takes.
    #[must_use]
    pub const fn max_depth(self) -> u32 {
        self.max_depth
    }
}

impl Default for VolPathIntegrator {
    /// pbrt-v4's defaults: `maxdepth` 5.
    fn default() -> Self {
        Self::new(5)
    }
}

impl RayIntegrator for VolPathIntegrator {
    /// pbrt-v4 `VolPathIntegrator::Li`. The ray escapes, and there is no
    /// infinite light to add: zero radiance, with no sample drawn.
    fn li<R: Recorder>(
        &self,
        _view: &SceneView<'_>,
        _ray: &Ray,
        _lambda: &mut SampledWavelengths,
        _state: &mut PixelSampleState<'_, R>,
    ) -> SampledSpectrum {
        SampledSpectrum::splat(0.0)
    }
}
