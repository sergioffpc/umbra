# umbra-scene

The owning Scene: its per-type arenas with typed `u32` handles (ADR 0008),
the `SceneBuilder` that fills them, the deterministic BVH construction, and
the Render Job Options. `SceneBuilder` is the seam that makes a Scene
independent of the file format it was read from: the parser is one adapter,
and tests that build Scenes directly are the other.

It is a `std` crate, outside the core. It depends on `umbra-integrators`, with
the core's `serde` feature on so that a Scene can be replicated to the Workers,
and `umbra-parser` and `umbra-render` depend on it (arc42 §5.1). Its contract
is sketched in arc42 §5.3.

```rust
use umbra_math::Point2i;
use umbra_scene::{FilmParameters, RenderJobOptions, SceneBuilder};

// pbrt-v4's defaults, but a small film.
let mut builder = SceneBuilder::new();
builder.film(FilmParameters { resolution: Point2i::new(64, 48) });
let scene = builder.build()?;
assert_eq!(scene.full_resolution(), Point2i::new(64, 48));

// What per-sample code reads of it.
let _view = scene.view();
let options = RenderJobOptions::default();
assert_eq!(options.tile_size.get(), 32);
# Ok::<(), umbra_scene::SceneError>(())
```

## What is in it

| Module         | Contents                                                                                                                                                                                                                                      |
| -------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `scene`        | `SceneBuilder`, with one parameter type per directive it supports (`FilmParameters`, `PerspectiveCameraParameters`, `GaussianFilterParameters`, `HaltonSamplerParameters`, and `VolPathIntegrator`), and the `Scene` it builds, with `view()` |
| `arena`        | `Arena<T>`: the objects of one type, which mints a `Handle<T>` per object and refuses more than a `u32` can index                                                                                                                             |
| `options`      | `RenderJobOptions` (seed, Tile size, Pixel Samples per Pass, Pixel Samples per pixel) and `Seed`                                                                                                                                              |
| `content_hash` | `ContentHash`: the SHA-256 of the files a Scene was read from, which the parser computes and the Reproduction Record carries                                                                                                                  |
| `error`        | `SceneError`                                                                                                                                                                                                                                  |

Not here yet: Shapes, lights and materials and their arenas, from
[#79](https://github.com/sergioffpc/umbra/issues/79) on, with the BVH; the
other `RGBFilm` parameters with
[#84](https://github.com/sergioffpc/umbra/issues/84); serialising a Scene for
replication, with remote Workers.

## Design decisions

### Settings in, core objects out

`SceneBuilder` holds the settings of each directive as a Scene File gives
them, every one at pbrt-v4's default until set, and `build` turns them into
the core's objects: the camera's matrices, the filter's sampling tables, the
sensor. So a setter's order does not matter (the camera needs the film's
resolution, for one), and `build` is the one place that validates. The one
default that is not pbrt-v4's is the sampler: Umbra has only `halton`,
where pbrt-v4 defaults to `zsobol`.

### Per-type arenas before their first type

`Arena<T>` and `Handle<T>` exist before the Scene holds an arena: a
camera-only Scene has no Scene-resident objects, and the camera, the filter
and the sampler are one per Render Job, held by value (ADR 0008). Shapes
bring the first arena. `Handle<T>` is `umbra-geometry`'s, since the core's
interactions will carry handles.

### The sampler's seed and size are Render Job Options

`Scene` names the sampler's randomisation; the number of Pixel Samples and
the seed are `RenderJobOptions`, as pbrt-v4's `--spp` and `--seed` override
the Scene File's. The digit permutations a seed chooses are computed by
`umbra-render`'s `Renderer`, once per Render Job.

### The output colour space is the Scene's

`build` computes the matrix from the sensor's RGB to sRGB, the only output
colour space so far, once; the Film reads it from the Scene to finish its
image (pbrt-v4 `RGBFilm::outputRGBFromSensorRGB`).

## Things to know when using it

- **`build` consumes the builder**, and the Scene is immutable from then on;
  threads share `&Scene`.
- **A reversed shutter interval is swapped**, as pbrt-v4 does (with a
  warning there).
- **The default sensor's imaging ratio is the exposure time**: the shutter
  interval's length at an ISO of 100.

## Tests

`tests/umbra_scene/` checks that each handle of an `Arena` gets its own
object, and that `SceneBuilder::build` refuses a film without pixels and a
filter too small to tabulate. Rendering a built Scene is tested in
`umbra-render`.
