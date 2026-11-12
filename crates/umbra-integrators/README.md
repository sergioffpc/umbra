# umbra-integrators

How one Pixel Sample is computed from start to end: the per-Pixel-Sample body
of each integrator, the `SceneView` it reads the Scene through (borrowed arena
slices, never `&Scene`), and the `Recorder` trait through which a Pixel Trace
watches the same code path that renders (ADR 0011).

It is a core crate: `#![no_std]` without `alloc` (ADR 0014, ADR 0015), and the
top of the core. It depends on `umbra-shading` and `umbra-camera`, and
`umbra-scene` and `umbra-render` depend on it (arc42 §5.1, §5.2). It covers
PBR 4e chapters 13–15.

A Scene is built by `umbra-scene`, and its Pixel Samples are computed one at
a time:

```rust
use umbra_camera::{
    CameraBaseParameters, CameraKind, CameraTransform, FilterKind, GaussianFilter,
    PerspectiveCamera, PixelSensor, ThinLens,
};
use umbra_geometry::Bounds2f;
use umbra_integrators::{
    NoRecorder, PixelSampleId, PixelSampleState, SceneView, VolPathIntegrator,
    evaluate_pixel_sample,
};
use umbra_math::{Point2f, Point2i, Transform, Vector2f};
use umbra_sampling::{HaltonRandomizer, HaltonSampler};

let base = CameraBaseParameters {
    camera_transform: CameraTransform::new(&Transform::default()),
    shutter_open: 0.0,
    shutter_close: 1.0,
};
let screen = Bounds2f::new(Point2f::new(-1.0, -1.0), Point2f::new(1.0, 1.0));
let lens = ThinLens { radius: 0.0, focal_distance: 1e6 };
let camera: CameraKind =
    PerspectiveCamera::new(&base, Point2i::new(8, 8), 90.0, screen, lens).into();
let filter: FilterKind<Vec<f32>> =
    GaussianFilter::new_in(Vector2f::new(1.5, 1.5), 0.5, |n| vec![0.0; n]).into();
let sensor = PixelSensor::cie1931(1.0);
let view = SceneView::new(&camera, filter.view(), &sensor);

// A thread's state, reused for every Pixel Sample it computes.
let mut state = PixelSampleState {
    sampler: HaltonSampler::new(4, Point2i::new(8, 8), HaltonRandomizer::None).into(),
    recorder: NoRecorder,
};
let id = PixelSampleId { pixel: Point2i::new(3, 4), sample_index: 2 };
let sample = evaluate_pixel_sample(&VolPathIntegrator::default(), &view, id, &mut state);

// Nothing to see yet: the Scene has no Shape and no light.
assert_eq!((sample.rgb.r, sample.rgb.g, sample.rgb.b), (0.0, 0.0, 0.0));
assert!(sample.weight > 0.0);
```

## What is in it

| Module (pbrt-v4 source)                                                        | Contents                                                                                                                                                                                                  |
| ------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `view`                                                                         | `SceneView`: the camera, the filter's tables and the sensor, borrowed from a Scene                                                                                                                        |
| `pixel_sample` (`RayIntegrator::EvaluatePixelSample` of `cpu/integrators.cpp`) | `PixelSampleId`, `PixelSampleState` (a thread's sampler and recorder), the `RayIntegrator` trait, `FilmSample`, and `evaluate_pixel_sample`: wavelengths, camera sample, camera ray, radiance, sensor RGB |
| `recorder`                                                                     | The `Recorder` trait, one method per step of a Pixel Sample, and `NoRecorder`, zero-sized, which records nothing                                                                                          |
| `volpath` (`VolPathIntegrator` of `cpu/integrators.h`)                         | `VolPathIntegrator` and its `max_depth`                                                                                                                                                                   |

Everything is re-exported at the crate root.

Not here yet: intersections, emission and shading in `VolPathIntegrator`,
with the Shapes and lights of
[#79](https://github.com/sergioffpc/umbra/issues/79) and
[#80](https://github.com/sergioffpc/umbra/issues/80); the `TraceRecorder`
of Pixel Traces, which is `umbra-render`'s, with
[#88](https://github.com/sergioffpc/umbra/issues/88); and leaving out
Anomalous Pixel Samples, with
[#85](https://github.com/sergioffpc/umbra/issues/85). Until then a NaN or
infinite radiance reaches the Film, where pbrt-v4 would zero it.

## Design decisions

### One function per Pixel Sample, shared by rendering and tracing

`evaluate_pixel_sample` is pbrt-v4's `EvaluatePixelSample` with
`StartPixelSample` in front: given the Pixel Sample's id, it restarts the
sampler and computes everything from the wavelengths to what the Film
receives. It is a pure function of the Scene, the seed and the id
(ADR 0022). Rendering calls it with `NoRecorder`, a Pixel Trace with a
recorder that records (ADR 0011): the same code, so the trace is what was
rendered. `NoRecorder`'s methods are empty and `#[inline(always)]`, and its
size is asserted to be 0.

### Values in, values out

Every step takes and returns values (ADR 0028): the sampler and the recorder
are a `PixelSampleState` a thread owns and passes by `&mut`, and the Film's
part is a returned `FilmSample` (the sensor's RGB and the filter weight),
which the caller sums in `f64` (ADR 0001). Nothing here knows of Tiles,
threads or the Film, so the GPU's wavefront can call the same steps.

### `RayIntegrator` is generic, not `dyn`

`RayIntegrator::li` is generic over the recorder, so it cannot be a trait
object. The `dyn Integrator` of arc42 §5.3 is `umbra-render`'s, called once
per Work Unit around a loop that calls `evaluate_pixel_sample` statically
(ADR 0006).

## Things to know when using it

- **`evaluate_pixel_sample` restarts the sampler**: callers do not call
  `start_pixel_sample` themselves.
- **A `SceneView` is cheap to build** and holds only borrows; build it once
  per Work Unit and share it between threads.

## Tests

No tests of their own yet: `umbra-render`'s tests render through
`evaluate_pixel_sample` and `VolPathIntegrator` (a black frame, identical
Work Results on any number of threads), and the counting-allocator test in
`crates/umbra/tests/` checks that a Pixel Sample allocates nothing. The
doctest above computes one Pixel Sample.
