# umbra-cpu

The CPU's renderer: the `Renderer` that turns a Work Unit into a Work Result
on the cores of one NUMA node, splitting its rows over `rayon` and computing
each Pixel Sample with the core's `evaluate_pixel_sample` (ADR 0033). Pixel
Traces re-execute Pixel Samples through the same code path (ADR 0011).

It is a `std` crate, outside the core. It depends on `umbra-render` for the
contract every Render Device shares, and on `umbra-integrators` and
`umbra-scene`; `umbra-cluster` depends on it (arc42 §5.1). Its contract is
sketched in arc42 §5.3. `umbra-gpu`'s `Renderer` has the same shape.

```rust
use umbra_cpu::Renderer;
use umbra_math::Point2i;
use umbra_render::{Acceptance, Cancellation, Film, Meter, Rendered};
use umbra_scene::{FilmParameters, RenderJobOptions, SceneBuilder};

let mut builder = SceneBuilder::new();
builder.film(FilmParameters { resolution: Point2i::new(48, 32) });
let scene = builder.build()?;
let options = RenderJobOptions::default();

let renderer = Renderer::new(&scene, &options)?;
let mut film = Film::new(&scene, &options)?;
let cancel = Cancellation::new();
for unit in renderer.decomposition().work_units() {
    if let Rendered::Done(result) = renderer.render(unit, &cancel, &Meter::new()).rendered {
        assert_eq!(film.accept(&result), Acceptance::Merged);
    }
}
assert!(film.is_complete());
# Ok::<(), Box<dyn std::error::Error>>(())
```

## What is in it

| Module     | Contents                                                                                                                                                                       |
| ---------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `renderer` | `Renderer`, which renders a `WorkUnit` into `Rendered::Done` with its Work Result, or `Rendered::Cancelled`, on rayon's pool, and `Render`, which adds the threads' time on it |

Not here yet: Pixel Traces and their `TraceRecorder`, with
[#88](https://github.com/sergioffpc/umbra/issues/88).

## Design decisions

### `dyn Integrator` once per Work Unit, a static loop inside

The Renderer holds its integrator as a private `dyn Integrator`, called once
per Work Unit (ADR 0006). Behind it, `PixelLoop` is generic over the core's
`RayIntegrator`: it splits the Tile's rows with rayon's `par_chunks_mut`,
and each row calls `evaluate_pixel_sample` with `NoRecorder` for every
Pixel Sample of every pixel, in ascending sample index (ADR 0022). Threads
split a Work Unit by row, never within a pixel, so a Work Result does not
depend on the thread count.

### Allocation per Work Unit, never per Pixel Sample

A render allocates the Work Result's buffer and nothing else (ADR 0008): a
row's thread state is the sampler, a small value copied from the Render
Job's, and `NoRecorder`. The digit permutations of the seed are allocated
once, by `Renderer::new`.

## Things to know when using it

- **`render` may run for several Work Units at once**, from any threads:
  the Renderer is `Sync`. Each render uses rayon's current pool, so
  `ThreadPool::install` sets the thread count.
- **Cancellation is checked between rows**: a render already inside a row
  finishes it before returning `Rendered::Cancelled`.
- **`render` panics** on a Work Unit outside the Render Job: it is a bug.

## Tests

`tests/umbra_cpu/renderer.rs` tests the Renderer over a camera-only Scene
from `SceneBuilder` (spec #72): every pixel is black with positive filter
weights; Work Results and the single-Pass Film hash are the same on 1 and 4
threads; a Work Unit renders the same again in another order; a cancelled
render returns no Work Result.

The counting-allocator test of ADR 0008, which renders through this crate,
is in `crates/umbra/tests/allocation.rs`.

Run them with `cargo nextest run -p umbra-cpu`.
