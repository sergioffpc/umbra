//! What per-sample code reads of the Scene (ADR 0014): borrowed, never
//! `&Scene`.

use umbra_camera::{CameraKind, FilterKind, PixelSensor};

/// The parts of a Scene one Pixel Sample reads, borrowed from it: the
/// camera, the filter's sampling tables and the sensor. Shapes, lights and
/// the BVH join it as arena slices with the features that use them.
///
/// `umbra-scene`'s `Scene::view` builds it; the GPU will build one over
/// device copies of the same arenas (ADR 0028).
#[derive(Debug, Clone)]
pub struct SceneView<'a> {
    camera: &'a CameraKind,
    filter: FilterKind<&'a [f32]>,
    sensor: PixelSensor<&'a [f32]>,
}

impl<'a> SceneView<'a> {
    /// The view of a Scene with this camera, filter and sensor.
    #[must_use]
    pub fn new(
        camera: &'a CameraKind,
        filter: FilterKind<&'a [f32]>,
        sensor: PixelSensor<&'a [f32]>,
    ) -> Self {
        Self {
            camera,
            filter,
            sensor,
        }
    }

    /// The camera.
    #[must_use]
    pub fn camera(&self) -> &'a CameraKind {
        self.camera
    }

    /// The pixel filter.
    #[must_use]
    pub fn filter(&self) -> &FilterKind<&'a [f32]> {
        &self.filter
    }

    /// The film's sensor.
    #[must_use]
    pub fn sensor(&self) -> &PixelSensor<&'a [f32]> {
        &self.sensor
    }
}
