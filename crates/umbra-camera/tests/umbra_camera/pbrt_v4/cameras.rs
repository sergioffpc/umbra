//! The `known_values` tests of `PerspectiveCamera` and `get_camera_sample`
//! against `known_values.rs`. pbrt-v4 has no camera tests to port.

use umbra_camera::{
    Camera, CameraBaseParameters, CameraKind, CameraSample, CameraTransform, FilterKind,
    GaussianFilter, PerspectiveCamera, ThinLens, get_camera_sample,
};
use umbra_geometry::Bounds2f;
use umbra_math::{Point2f, Point2i, Point3f, Transform, Vector2f, Vector3f, look_at};
use umbra_sampling::{DigitPermutations, HaltonRandomizer, HaltonSampler, Sampler, SamplerKind};
use umbra_spectrum::SampledWavelengths;

use super::known_values::{CAMERA_SAMPLE, PERSPECTIVE_PINHOLE, PERSPECTIVE_THIN_LENS};
use super::{assert_scaled_ulps, assert_ulps, floats};

/// pbrt-v4's `PerspectiveCamera` as `known_values.cpp` builds it: the
/// default screen window of the film's aspect ratio.
fn camera(
    world_from_camera: &Transform,
    resolution: Point2i,
    fov: f32,
    lens: ThinLens,
) -> CameraKind {
    let resolution_f = Point2f::from(resolution);
    let frame = resolution_f.x / resolution_f.y;
    let screen = if frame > 1.0 {
        Bounds2f::new(Point2f::new(-frame, -1.0), Point2f::new(frame, 1.0))
    } else {
        Bounds2f::new(
            Point2f::new(-1.0, -1.0 / frame),
            Point2f::new(1.0, 1.0 / frame),
        )
    };
    let base = CameraBaseParameters {
        camera_transform: CameraTransform::new(world_from_camera),
        shutter_open: 0.0,
        shutter_close: 1.0,
    };
    PerspectiveCamera::new(&base, resolution, fov, screen, lens).into()
}

/// Checks the rays of `camera` against the table pbrt-v4 computed.
#[track_caller]
fn check_rays(camera: &CameraKind, expected: &[([u32; 4], [u32; 6])], max_ulps: u16) {
    let lambda = SampledWavelengths::sample_visible(0.5);
    for &(sample, ray) in expected {
        let [fx, fy, lx, ly] = floats(sample);
        let sample = CameraSample {
            p_film: Point2f::new(fx, fy),
            p_lens: Point2f::new(lx, ly),
            ..CameraSample::default()
        };
        let r = camera
            .generate_ray(sample, &lambda)
            .expect("a perspective camera ray")
            .ray;
        let what = format!("GenerateRay({sample:?})");
        let [ox, oy, oz, dx, dy, dz] = ray;
        assert_scaled_ulps([r.o.x, r.o.y, r.o.z], [ox, oy, oz], max_ulps, &what);
        assert_scaled_ulps([r.d.x, r.d.y, r.d.z], [dx, dy, dz], max_ulps, &what);
    }
}

#[test]
fn known_values_perspective_pinhole() {
    let lens = ThinLens {
        radius: 0.0,
        focal_distance: 1e6,
    };
    let camera = camera(&Transform::default(), Point2i::new(16, 9), 90.0, lens);
    check_rays(&camera, PERSPECTIVE_PINHOLE, 4);
}

#[test]
fn known_values_perspective_thin_lens() {
    let camera_from_world = look_at(
        Point3f::new(1.0, 2.0, 3.0),
        Point3f::new(0.0, 0.5, 0.0),
        Vector3f::new(0.0, 1.0, 0.0),
    )
    .expect("a camera's look-at");
    let lens = ThinLens {
        radius: 0.1,
        focal_distance: 5.0,
    };
    let camera = camera(
        &camera_from_world.inverse(),
        Point2i::new(32, 64),
        45.0,
        lens,
    );
    check_rays(&camera, PERSPECTIVE_THIN_LENS, 4);
}

#[test]
fn known_values_camera_sample() {
    let permutations = DigitPermutations::new_in(0, |n| vec![0; n]);
    let randomizer = HaltonRandomizer::PermuteDigits(permutations.view());
    let mut sampler = SamplerKind::from(HaltonSampler::new(16, Point2i::new(64, 32), randomizer));
    let filter: FilterKind<Vec<f32>> =
        GaussianFilter::new_in(Vector2f::new(1.5, 1.5), 0.5, |n| vec![0.0; n]).into();
    for &(x, y, sample_index, expected) in CAMERA_SAMPLE {
        let pixel = Point2i::new(x, y);
        sampler.start_pixel_sample(pixel, sample_index, 0);
        let cs = get_camera_sample(&mut sampler, pixel, &filter.view());
        let actual = [
            cs.p_film.x,
            cs.p_film.y,
            cs.time,
            cs.p_lens.x,
            cs.p_lens.y,
            cs.filter_weight,
        ];
        assert_ulps(
            actual,
            expected,
            8,
            &format!("GetCameraSample({pixel:?}, {sample_index})"),
        );
    }
}
