//! Cameras (pbrt-v4 `src/pbrt/base/camera.h`, `cameras.h`, `cameras.cpp`):
//! how a camera sample becomes a ray in rendering space, through the closed
//! [`CameraKind`] (ADR 0006).

use umbra_geometry::{Bounds2f, Ray};
use umbra_math::{
    Point2f, Point2i, Point3f, Transform, Vector2f, Vector3f, lerp, perspective, scale, translate,
};
use umbra_sampling::{Sampler, sample_uniform_disk_concentric};
use umbra_spectrum::{SampledSpectrum, SampledWavelengths};

use crate::filter::{Filter, FilterKind};

/// Where on the film, on the lens and in the shutter interval a camera ray
/// starts, and the filter's weight for it.
///
/// PBR 4e §5.1.1, pbrt-v4 `CameraSample`.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct CameraSample {
    /// The point on the film, in raster coordinates.
    pub p_film: Point2f,
    /// The point on the lens, in `[0, 1)²`.
    pub p_lens: Point2f,
    /// The time, in `[0, 1)`, mapped into the shutter interval.
    pub time: f32,
    /// The weight of the filter at `p_film`.
    pub filter_weight: f32,
}

/// The camera sample of the current Pixel Sample of `sampler` in `pixel`:
/// the film position importance-sampled from `filter` around the pixel's
/// centre, then the time and the lens position.
///
/// PBR 4e §8.8.4, pbrt-v4 `GetCameraSample`.
pub fn get_camera_sample<S: AsRef<[f32]>>(
    sampler: &mut impl Sampler,
    pixel: Point2i,
    filter: &FilterKind<S>,
) -> CameraSample {
    let fs = filter.sample(sampler.get_pixel_2d());
    CameraSample {
        p_film: Point2f::from(pixel) + fs.p + Vector2f::new(0.5, 0.5),
        time: sampler.get_1d(),
        p_lens: sampler.get_2d(),
        filter_weight: fs.weight,
    }
}

/// A ray leaving the camera, and the weight of the radiance it carries
/// back.
///
/// PBR 4e §5.1, pbrt-v4 `CameraRay`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CameraRay {
    /// The ray, in rendering space.
    pub ray: Ray,
    /// The weight of the radiance arriving along it.
    pub weight: SampledSpectrum,
}

/// Where the camera is: the transformation from camera space to the space
/// rendering happens in, and from that space to world space.
///
/// Rendering space is pbrt-v4's default, camera-world: world space
/// translated so the camera sits at its origin. The camera does not move
/// during the shutter interval; animated transforms arrive with a Scene that
/// needs them.
///
/// PBR 4e §5.1.1, pbrt-v4 `CameraTransform`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CameraTransform {
    render_from_camera: Transform,
    world_from_render: Transform,
}

impl CameraTransform {
    /// The camera at `world_from_camera`, rendering in camera-world space.
    ///
    /// pbrt-v4 `CameraTransform(worldFromCamera)` with
    /// `RenderingCoordinateSystem::CameraWorld`.
    #[must_use]
    pub fn new(world_from_camera: &Transform) -> Self {
        let p_camera = world_from_camera.apply(Point3f::new(0.0, 0.0, 0.0));
        let world_from_render = translate(Vector3f::from(p_camera));
        let render_from_camera = world_from_render.inverse() * *world_from_camera;
        Self {
            render_from_camera,
            world_from_render,
        }
    }

    /// pbrt-v4 `CameraTransform::RenderFromCamera()`.
    #[must_use]
    pub fn render_from_camera(&self) -> &Transform {
        &self.render_from_camera
    }

    /// pbrt-v4 `CameraTransform::WorldFromRender()`.
    #[must_use]
    pub fn world_from_render(&self) -> &Transform {
        &self.world_from_render
    }
}

/// What every camera has: where it is and when its shutter is open.
///
/// pbrt-v4 `CameraBaseParameters`; its film and medium are not held here.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CameraBaseParameters {
    /// Where the camera is.
    pub camera_transform: CameraTransform,
    /// When the shutter opens.
    pub shutter_open: f32,
    /// When the shutter closes.
    pub shutter_close: f32,
}

/// A thin lens: its radius, and the distance to the plane in focus.
///
/// pbrt-v4's `lensradius` and `focaldistance`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ThinLens {
    /// The lens radius; 0 for a pinhole.
    pub radius: f32,
    /// The distance from the lens to the plane in focus.
    pub focal_distance: f32,
}

/// The perspective projection camera, with a thin lens for depth of field.
///
/// PBR 4e §5.2.2, pbrt-v4 `PerspectiveCamera`. Only what generating a ray
/// needs; the importance functions (`We`, `PDF_We`, `SampleWi`) and ray
/// differentials arrive with the features that use them.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct PerspectiveCamera {
    base: CameraBaseParameters,
    camera_from_raster: Transform,
    lens: ThinLens,
}

impl PerspectiveCamera {
    /// The camera with a field of view of `fov` degrees across the shorter
    /// side of `screen_window`, mapped onto a film of `full_resolution`
    /// pixels.
    ///
    /// pbrt-v4 `PerspectiveCamera(baseParameters, fov, screenWindow,
    /// lensRadius, focalDist)` and `ProjectiveCamera`'s constructor.
    #[must_use]
    pub fn new(
        base: &CameraBaseParameters,
        full_resolution: Point2i,
        fov: f32,
        screen_window: Bounds2f,
        lens: ThinLens,
    ) -> Self {
        let screen_from_camera = perspective(fov, 1e-2, 1000.0);
        let Bounds2f { p_min, p_max } = screen_window;
        let ndc_from_screen = scale(1.0 / (p_max.x - p_min.x), 1.0 / (p_max.y - p_min.y), 1.0)
            * translate(Vector3f::new(-p_min.x, -p_max.y, 0.0));
        let resolution = Point2f::from(full_resolution);
        let raster_from_ndc = scale(resolution.x, -resolution.y, 1.0);
        let raster_from_screen = raster_from_ndc * ndc_from_screen;
        let camera_from_raster = screen_from_camera.inverse() * raster_from_screen.inverse();
        Self {
            base: *base,
            camera_from_raster,
            lens,
        }
    }
}

/// How a camera turns a camera sample into a ray.
///
/// PBR 4e §5.1, pbrt-v4 `Camera`.
pub trait Camera {
    /// The ray for `sample`, in rendering space, or `None` if there is
    /// none.
    ///
    /// pbrt-v4 `Camera::GenerateRay`.
    fn generate_ray(&self, sample: CameraSample, lambda: &SampledWavelengths) -> Option<CameraRay>;

    /// The time at `u` of the way through the shutter interval.
    ///
    /// pbrt-v4 `CameraBase::SampleTime`.
    fn sample_time(&self, u: f32) -> f32;
}

impl Camera for PerspectiveCamera {
    fn generate_ray(
        &self,
        sample: CameraSample,
        _lambda: &SampledWavelengths,
    ) -> Option<CameraRay> {
        let p_film = Point3f::new(sample.p_film.x, sample.p_film.y, 0.0);
        let p_camera = self.camera_from_raster.apply(p_film);
        let mut ray = Ray::new(
            Point3f::new(0.0, 0.0, 0.0),
            Vector3f::from(p_camera).normalize(),
        );
        if self.lens.radius > 0.0 {
            let p_lens = sample_uniform_disk_concentric(sample.p_lens) * self.lens.radius;
            let ft = self.lens.focal_distance / ray.d.z;
            let p_focus = ray.at(ft);
            ray.o = Point3f::new(p_lens.x, p_lens.y, 0.0);
            ray.d = (p_focus - ray.o).normalize();
        }
        Some(CameraRay {
            ray: ray.transform(self.base.camera_transform.render_from_camera()),
            weight: SampledSpectrum::splat(1.0),
        })
    }

    fn sample_time(&self, u: f32) -> f32 {
        lerp(u, self.base.shutter_open, self.base.shutter_close)
    }
}

/// The cameras, one per Render Job, held by value and matched on every
/// call (ADR 0006). Only pbrt-v4's `perspective` so far.
///
/// pbrt-v4 `Camera`'s tagged pointer.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum CameraKind {
    /// pbrt-v4 `"perspective"`.
    Perspective(PerspectiveCamera),
}

impl From<PerspectiveCamera> for CameraKind {
    fn from(camera: PerspectiveCamera) -> Self {
        Self::Perspective(camera)
    }
}

impl Camera for CameraKind {
    #[inline]
    fn generate_ray(&self, sample: CameraSample, lambda: &SampledWavelengths) -> Option<CameraRay> {
        match self {
            Self::Perspective(c) => c.generate_ray(sample, lambda),
        }
    }

    #[inline]
    fn sample_time(&self, u: f32) -> f32 {
        match self {
            Self::Perspective(c) => c.sample_time(u),
        }
    }
}
