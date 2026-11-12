# umbra-render

Rendering in one process: the `Renderer` that turns a Work Unit into a Work
Result and re-runs Pixel Samples as Pixel Traces through the same code path,
the Film that merges Work Results as they arrive (ADR 0022), and the EXR final
image that carries its Reproduction Record (ADR 0020).

It is a `std` crate, outside the core. It depends on `umbra-integrators` and
`umbra-scene`, and `umbra-cluster` and the `umbra` binary depend on it
(arc42 §5.1). Its contract is sketched in arc42 §5.3.

```rust
use umbra_math::Point2i;
use umbra_render::{Acceptance, Cancellation, Film, Rendered, Renderer};
use umbra_scene::{FilmParameters, RenderJobOptions, SceneBuilder};

let mut builder = SceneBuilder::new();
builder.film(FilmParameters { resolution: Point2i::new(48, 32) });
let scene = builder.build()?;
let options = RenderJobOptions::default();

let renderer = Renderer::new(&scene, &options)?;
let mut film = Film::new(&scene, &options)?;
let cancel = Cancellation::new();
for unit in renderer.decomposition().work_units() {
    if let Rendered::Done(result) = renderer.render(unit, &cancel) {
        assert_eq!(film.accept(&result), Acceptance::Merged);
    }
}
assert!(film.is_complete());
# Ok::<(), Box<dyn std::error::Error>>(())
```

## What is in it

| Module          | Contents                                                                                                                                                                                |
| --------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `decomposition` | `Decomposition`: the Tiles and Passes of a Render Job, its `WorkUnit`s (`TileIndex`, `PassIndex`) Pass by Pass in raster order, each Tile's `TileBounds` and each Pass's sample indices |
| `error`         | `Error`: why a Render Job cannot be cut into Work Units                                                                                                                                 |
| `renderer`      | `Renderer`, which renders a `WorkUnit` into `Rendered::Done` with its Work Result, or `Rendered::Cancelled`; `Cancellation`                                                             |
| `work_result`   | `WorkResult`: one `PixelSum` per pixel of a Tile, the `f64` sums of filter-weighted sensor RGB and of filter weights                                                                    |
| `film`          | `Film`, which accepts each Work Result as `Acceptance::Merged` or `Acceptance::Duplicate`, its `FilmHash`, and `finish` into an `Image`                                                 |
| `image`         | `Image`, written in the format its path's extension names, and `write_atomically`                                                                                                       |
| `reproduction`  | `ReproductionRecord`, its versioned JSON, and the `BuildIdentity` it holds                                                                                                              |

Not here yet: Pixel Traces and their `TraceRecorder`, with
[#88](https://github.com/sergioffpc/umbra/issues/88); the Anomalous Pixel
Samples of a Work Result, with
[#85](https://github.com/sergioffpc/umbra/issues/85).

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

### Merge on arrival, duplicates discarded unread

`Film::accept` adds a Work Result's sums into the Film's as soon as it
arrives, whatever its order (ADR 0022), and remembers its Work Unit; a second
Work Result for the same Work Unit is discarded unread, so every Pixel
Sample counts once. With one Pass per Tile, every pixel gets one Work Result,
so the Film is the same in any order; with several, the order can change the
last bits of a pixel.

### The Film hash

`Film::hash` is FNV-1a over the bits of every pixel's `f64` sums in raster
order: it tells two Films apart, for the invariance tests and the Render Job
Report, and is not a security hash.

### An EXR whose bytes are a function of the image

`Image::write` takes the format from the path's extension, as `image`
maps it, and writes RGB as 32-bit floats. For EXR, `image` uses `exr`
without its `rayon` feature: the file is compressed in one thread, so its
bytes are a pure function of the image (ADR 0020). A format that cannot
hold RGB floats fails with an encoding error, and an unknown extension
with `InvalidInput`. `image`
writes no custom attributes, so the Reproduction Record is in the Render
Job Report, not in the file.

### Finishing as pbrt-v4 does

`Film::finish` narrows each pixel's `f64` sums to `f32`, divides by the
weight when it is not zero, and converts sensor RGB to sRGB with the
Scene's matrix (pbrt-v4 `RGBFilm::GetPixelRGB`). It panics before the last
Work Unit is merged: an image exists only once every Pass is (ADR 0004).

## Things to know when using it

- **`render` may run for several Work Units at once**, from any threads:
  the Renderer is `Sync`. Each render uses rayon's current pool, so
  `ThreadPool::install` sets the thread count.
- **Cancellation is checked between rows**: a render already inside a row
  finishes it before returning `Rendered::Cancelled`.
- **`Film::accept` panics** on a Work Result for a Work Unit outside the
  Render Job, or with the wrong number of pixels: either is a bug.
- **`write_atomically` leaves the target as it was on failure**, and syncs
  both the file and its directory.
- **A Work Result's pixels are row by row across its Tile**, and the Tiles
  at the right and bottom edges are cut short by the image.

## Tests

`tests/umbra_render/` tests the crate at its two seams (spec #72):

- `film.rs`, the Film's acceptance: single-Pass Work Results merged in
  several orders give the same Film and hash; a duplicate with other sums
  leaves the Film unchanged; a Work Result lands on its Tile's pixels; the
  Passes of a Tile add up; a finished Film is each pixel's weighted mean in
  sRGB, against the BT.709 matrix.
- `renderer.rs`, the Renderer over a camera-only Scene from `SceneBuilder`:
  every pixel is black with positive filter weights; Work Results and the
  single-Pass Film hash are the same on 1 and 4 threads; a Work Unit renders
  the same again in another order; a cancelled render returns no Work Result.

The counting-allocator test of ADR 0008, which renders through this crate,
is in `crates/umbra/tests/allocation.rs`; the EXR's bytes and its
Reproduction Record are tested through the binary, in
`crates/umbra/tests/render/`.

Run them with `cargo nextest run -p umbra-render`.
