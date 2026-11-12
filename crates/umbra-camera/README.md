# umbra-camera

How a Pixel Sample becomes a ray, and radiance becomes sensor RGB: the Cameras
behind their closed `CameraKind` (ADR 0006), the Filters behind their closed
`FilterKind`, the pixel sensor, and camera sample generation.

It is a core crate: `#![no_std]` without `alloc` (ADR 0014, ADR 0015). It
depends on `umbra-geometry` and `umbra-spectrum`, and on `umbra-math` and
`umbra-sampling` below them, and `umbra-integrators` depends on it (arc42
§5.1, §5.2). It covers PBR 4e chapter 5 and §8.8.

```rust
use umbra_camera::{
    Camera, CameraBaseParameters, CameraKind, CameraTransform, FilterKind, GaussianFilter,
    PerspectiveCamera, PixelSensor, ThinLens, get_camera_sample,
};
use umbra_geometry::Bounds2f;
use umbra_math::{Point2f, Point2i, Transform, Vector2f};
use umbra_sampling::{HaltonRandomizer, HaltonSampler, Sampler, SamplerKind};
use umbra_spectrum::{SampledSpectrum, SampledWavelengths};

// pbrt-v4's default camera and filter, for a 64 × 64 film.
let base = CameraBaseParameters {
    camera_transform: CameraTransform::new(&Transform::default()),
    shutter_open: 0.0,
    shutter_close: 1.0,
};
let screen = Bounds2f::new(Point2f::new(-1.0, -1.0), Point2f::new(1.0, 1.0));
let lens = ThinLens { radius: 0.0, focal_distance: 1e6 };
let camera: CameraKind =
    PerspectiveCamera::new(&base, Point2i::new(64, 64), 90.0, screen, lens).into();
let filter: FilterKind<Vec<f32>> =
    GaussianFilter::new_in(Vector2f::new(1.5, 1.5), 0.5, |n| vec![0.0; n]).into();

// One Pixel Sample of pixel (10, 20): a camera sample, and its ray.
let mut sampler = SamplerKind::from(HaltonSampler::new(16, Point2i::new(64, 64), HaltonRandomizer::None));
let pixel = Point2i::new(10, 20);
sampler.start_pixel_sample(pixel, 0, 0);
let lambda = SampledWavelengths::sample_visible(sampler.get_1d());
let sample = get_camera_sample(&mut sampler, pixel, &filter.view());
let ray = camera.generate_ray(sample, &lambda).unwrap();

// The radiance arriving along it, in the default sensor's RGB.
let rgb = PixelSensor::cie1931(1.0).to_sensor_rgb(SampledSpectrum::splat(0.0), &lambda);
assert_eq!((rgb.r, rgb.g, rgb.b), (0.0, 0.0, 0.0));
assert!(ray.ray.d.z > 0.0);
```

## What is in it

| Module (pbrt-v4 source)                                                                   | Contents                                                                                                                                                                                             |
| ----------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `camera` (`base/camera.h`, `cameras.h`, `cameras.cpp`, `GetCameraSample` of `samplers.h`) | `CameraSample`, `CameraRay`, `CameraTransform` (camera-world rendering space), `CameraBaseParameters`, `ThinLens`, `PerspectiveCamera`, the `Camera` trait and its `CameraKind`; `get_camera_sample` |
| `filter` (`base/filter.h`, `filters.h`, `filters.cpp`)                                    | `FilterSample`, the `Filter` trait and its `FilterKind`, `GaussianFilter`, and `FilterSampler`, the tabulated distribution filters are importance-sampled with, over storage the caller provides     |
| `sensor` (`PixelSensor` of `film.h`, `film.cpp`)                                          | `PixelSensor`: the default `cie1931` sensor, whose RGB is XYZ, and `to_sensor_rgb`                                                                                                                   |

Everything is re-exported at the crate root.

Not here yet: the other cameras and filters, the cameras' importance
functions (`We`, `PDF_We`, `SampleWi`) and ray differentials, animated
camera transforms and the other rendering spaces, with the features that use
them; the named camera sensors, white balance and the other `RGBFilm`
parameters with
[#84](https://github.com/sergioffpc/umbra/issues/84). The Film itself is not
a core type: `umbra-render` accumulates what `umbra-integrators` computes
with the sensor (ADR 0001).

## Design decisions

### Filters are importance-sampled, from a table in storage the caller provides

Each Pixel Sample draws its film position from the filter's distribution and
carries the filter's value over that PDF as its weight, so it contributes to
its own pixel only (ADR 0001). pbrt-v4 tabulates the filter at 32 points per
unit of radius into a `PiecewiseConstant2D`; so does `FilterSampler`, over
storage `S` that `new_in`'s `alloc` returns, twice: the table, then the
distribution. The core has no allocator (ADR 0014): a Scene owns a
`FilterKind<Vec<f32>>`, and per-sample code samples its `view()`, a
`FilterKind<&[f32]>`.

### Only what generating a ray needs

`PerspectiveCamera` keeps `cameraFromRaster`, the thin lens and its
`CameraBaseParameters`: everything `GenerateRay` reads, computed by
pbrt-v4's `ProjectiveCamera` constructor operation for operation. pbrt-v4's
camera holds its Film; Umbra's takes the film's full resolution instead, as
the core has no Film. The members used only by importance functions and ray
differentials come with them. The camera does not move during the shutter
interval, so `CameraTransform` holds static transforms, and rendering is in
pbrt-v4's default camera-world space; the camera sample's time is still
drawn, so the sampler's dimensions stay pbrt-v4's.

## Things to know when using it

- **Draw order matters.** `get_camera_sample` draws the filter's sample
  first (`get_pixel_2d`), then the time, then the lens position, as pbrt-v4
  does: another order gives another sequence than pbrt-v4's.
- **`CameraTransform::new` takes `world_from_camera`**, the inverse of the
  current transformation at a Scene File's `Camera` directive.
- **`PixelSensor::to_sensor_rgb` divides by the wavelengths' PDFs**: pass the
  wavelengths the radiance was computed at.

## Tests

`tests/umbra_camera/` is one integration-test crate, through the public
interface only:

- `pbrt_v4/filters_test.rs` ports `filters_test.cpp`'s
  `TEST(Filter, ZeroPastRadius)` and `TEST(Filter, Integral)` for the
  Gaussian filter, and checks `Evaluate`, `Integral` and `Sample` against
  pbrt-v4's values.
- `pbrt_v4/cameras.rs` checks `PerspectiveCamera`'s rays, pinhole and thin
  lens, and `get_camera_sample` with the Halton sampler and the default
  filter, against pbrt-v4's values; `pbrt_v4/sensor.rs` checks the default
  sensor's RGB.
- `pbrt_v4/known_values.rs` holds those values, generated by
  `known_values.cpp` linked against a pbrt-v4 build at the pinned commit
  (the file says how). pbrt-v4's build lets its compiler fuse multiplies and
  adds, and its transcendentals come from its libm, so the tests allow a few
  ulps; a ray's components are compared in ulps of its largest one, as a
  component near zero can come out of a cancellation.

Run them with `cargo nextest run -p umbra-camera`.
