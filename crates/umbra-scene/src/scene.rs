//! The Scene and the builder that is the only way to make one.

use umbra_camera::{
    CameraBaseParameters, CameraKind, CameraTransform, FilterKind, GaussianFilter,
    PerspectiveCamera, PixelSensor, ThinLens,
};
use umbra_geometry::Bounds2f;
use umbra_integrators::{SceneView, VolPathIntegrator};
use umbra_math::{Point2f, Point2i, Transform, Vector2f};
use umbra_sampling::RandomizeStrategy;

use crate::error::SceneError;

/// The film, as far as the Scene needs it: its full resolution. The other
/// `RGBFilm` parameters arrive with
/// [#84](https://github.com/sergioffpc/umbra/issues/84).
///
/// pbrt-v4 `Film "rgb"`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FilmParameters {
    /// The image's full resolution, in pixels.
    pub resolution: Point2i,
}

impl Default for FilmParameters {
    /// pbrt-v4's defaults: 1280 × 720.
    fn default() -> Self {
        Self {
            resolution: Point2i::new(1280, 720),
        }
    }
}

/// A perspective camera, as a Scene File gives it.
///
/// pbrt-v4 `Camera "perspective"` and `PerspectiveCamera::Create`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PerspectiveCameraParameters {
    /// Where the camera is: the inverse of the current transformation at
    /// the `Camera` directive.
    pub world_from_camera: Transform,
    /// The field of view across the shorter side of the screen window, in
    /// degrees.
    pub fov: f32,
    /// The thin lens; a radius of 0 is a pinhole.
    pub lens: ThinLens,
    /// The screen window, or `None` for pbrt-v4's default from the frame
    /// aspect ratio.
    pub screen_window: Option<Bounds2f>,
    /// The frame aspect ratio, or `None` for the film's.
    pub frame_aspect_ratio: Option<f32>,
    /// When the shutter opens.
    pub shutter_open: f32,
    /// When the shutter closes.
    pub shutter_close: f32,
}

impl Default for PerspectiveCameraParameters {
    /// pbrt-v4's defaults: at the origin looking down +z, a field of view of
    /// 90 degrees, a pinhole, and a shutter open from 0 to 1.
    fn default() -> Self {
        Self {
            world_from_camera: Transform::default(),
            fov: 90.0,
            lens: ThinLens {
                radius: 0.0,
                focal_distance: 1e6,
            },
            screen_window: None,
            frame_aspect_ratio: None,
            shutter_open: 0.0,
            shutter_close: 1.0,
        }
    }
}

impl PerspectiveCameraParameters {
    /// The shutter interval in order, as pbrt-v4 swaps a reversed one.
    fn shutter(&self) -> (f32, f32) {
        let (open, close) = (self.shutter_open, self.shutter_close);
        if close < open {
            (close, open)
        } else {
            (open, close)
        }
    }

    /// The screen window: the one given, or pbrt-v4's default for the frame
    /// aspect ratio.
    fn screen_window(&self, resolution: Point2i) -> Bounds2f {
        let resolution = Point2f::from(resolution);
        let frame = self
            .frame_aspect_ratio
            .unwrap_or(resolution.x / resolution.y);
        self.screen_window
            .unwrap_or_else(|| default_screen_window(frame))
    }
}

/// pbrt-v4's default screen window for the frame aspect ratio `frame`:
/// `[-frame, frame] × [-1, 1]` for a wide frame, `[-1, 1] × [-1/frame,
/// 1/frame]` for a tall one.
fn default_screen_window(frame: f32) -> Bounds2f {
    let half = if frame > 1.0 {
        Point2f::new(frame, 1.0)
    } else {
        Point2f::new(1.0, 1.0 / frame)
    };
    Bounds2f::new(Point2f::new(-half.x, -half.y), half)
}

/// A Gaussian pixel filter, as a Scene File gives it.
///
/// pbrt-v4 `PixelFilter "gaussian"`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct GaussianFilterParameters {
    /// The half-widths of its support.
    pub radius: Vector2f,
    /// Its standard deviation.
    pub sigma: f32,
}

impl Default for GaussianFilterParameters {
    /// pbrt-v4's defaults: a radius of 1.5 and a `sigma` of 0.5.
    fn default() -> Self {
        Self {
            radius: Vector2f::new(1.5, 1.5),
            sigma: 0.5,
        }
    }
}

/// A Halton sampler, as a Scene File gives it. Its number of Pixel Samples
/// and its seed are Render Job Options.
///
/// pbrt-v4 `Sampler "halton"`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct HaltonSamplerParameters {
    /// How the sequence is randomised; pbrt-v4's default permutes digits.
    pub randomize: RandomizeStrategy,
}

/// The format-independent way to make a [`Scene`]: the parser is one
/// adapter, tests that build Scenes directly are another (arc42 §5.3).
/// Every setting starts at pbrt-v4's default, but the sampler: Umbra has
/// only `halton` so far, where pbrt-v4 defaults to `zsobol`.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct SceneBuilder {
    film: FilmParameters,
    camera: PerspectiveCameraParameters,
    filter: GaussianFilterParameters,
    sampler: HaltonSamplerParameters,
    integrator: VolPathIntegrator,
}

impl SceneBuilder {
    /// A builder with every setting at its default.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Sets the film.
    pub fn film(&mut self, film: FilmParameters) -> &mut Self {
        self.film = film;
        self
    }

    /// Sets the camera.
    pub fn camera(&mut self, camera: &PerspectiveCameraParameters) -> &mut Self {
        self.camera = *camera;
        self
    }

    /// Sets the pixel filter.
    pub fn filter(&mut self, filter: GaussianFilterParameters) -> &mut Self {
        self.filter = filter;
        self
    }

    /// Sets the sampler.
    pub fn sampler(&mut self, sampler: HaltonSamplerParameters) -> &mut Self {
        self.sampler = sampler;
        self
    }

    /// Sets the integrator.
    pub fn integrator(&mut self, integrator: VolPathIntegrator) -> &mut Self {
        self.integrator = integrator;
        self
    }

    /// Freezes the settings into an immutable Scene.
    ///
    /// # Errors
    ///
    /// [`SceneError::EmptyResolution`] if the film has no pixel, and
    /// [`SceneError::FilterRadiusTooSmall`] if the filter cannot be
    /// tabulated.
    pub fn build(self) -> Result<Scene, SceneError> {
        let resolution = self.film.resolution;
        if resolution.x < 1 || resolution.y < 1 {
            return Err(SceneError::EmptyResolution { resolution });
        }
        let GaussianFilterParameters { radius, sigma } = self.filter;
        if radius.x * 32.0 < 1.0 || radius.y * 32.0 < 1.0 {
            return Err(SceneError::FilterRadiusTooSmall);
        }
        let (shutter_open, shutter_close) = self.camera.shutter();
        Ok(Scene {
            resolution,
            camera: self.build_camera(shutter_open, shutter_close),
            filter: GaussianFilter::new_in(radius, sigma, |n| vec![0.0; n]).into(),
            sensor: PixelSensor::cie1931(imaging_ratio(shutter_close - shutter_open)),
            randomize: self.sampler.randomize,
            integrator: self.integrator,
        })
    }

    /// pbrt-v4 `PerspectiveCamera::Create`.
    fn build_camera(&self, shutter_open: f32, shutter_close: f32) -> CameraKind {
        let camera = &self.camera;
        let base = CameraBaseParameters {
            camera_transform: CameraTransform::new(&camera.world_from_camera),
            shutter_open,
            shutter_close,
        };
        let resolution = self.film.resolution;
        let screen = camera.screen_window(resolution);
        PerspectiveCamera::new(&base, resolution, camera.fov, screen, camera.lens).into()
    }
}

/// The default sensor's imaging ratio: the exposure time times an ISO of
/// 100, over 100.
///
/// pbrt-v4 `PixelSensor::Create`.
fn imaging_ratio(exposure_time: f32) -> f32 {
    const ISO: f32 = 100.0;
    exposure_time * ISO / 100.0
}

/// The complete, validated description of what is rendered, independent of
/// the file it was read from (`GLOSSARY.md`). Immutable once built, and
/// shared between threads as `&Scene` (ADR 0008).
///
/// It holds no Shape, light or material yet: those arrive with their
/// arenas in [#79](https://github.com/sergioffpc/umbra/issues/79) and after.
#[derive(Debug, Clone, PartialEq)]
pub struct Scene {
    resolution: Point2i,
    camera: CameraKind,
    filter: FilterKind<Vec<f32>>,
    sensor: PixelSensor<'static>,
    randomize: RandomizeStrategy,
    integrator: VolPathIntegrator,
}

impl Scene {
    /// What per-sample code reads of the Scene (ADR 0014).
    #[must_use]
    pub fn view(&self) -> SceneView<'_> {
        SceneView::new(&self.camera, self.filter.view(), &self.sensor)
    }

    /// The image's full resolution, in pixels; both sides are positive.
    #[must_use]
    pub fn full_resolution(&self) -> Point2i {
        self.resolution
    }

    /// How the sampler randomises its sequence.
    #[must_use]
    pub fn randomize_strategy(&self) -> RandomizeStrategy {
        self.randomize
    }

    /// The integrator.
    #[must_use]
    pub fn integrator(&self) -> VolPathIntegrator {
        self.integrator
    }
}
