# umbra-render

What every Render Device shares (ADR 0033): the Work Units a Render Job is
cut into and the Work Results that answer them, how a render ends and the
request that cancels it, the Film that merges Work Results as they arrive
(ADR 0022), and the EXR final image that carries its Reproduction Record
(ADR 0020). The renderers are elsewhere: the CPU's in `umbra-cpu`, the GPU's
in `umbra-gpu`, both over this contract alone.

It is a `std` crate, outside the core. It depends on `umbra-integrators` and
`umbra-scene`, and on neither `rayon` nor anything from NVIDIA; `umbra-cpu`,
`umbra-gpu`, `umbra-cluster` and the `umbra` binary depend on it (arc42
§5.1). Its contract is sketched in arc42 §5.3.

```rust
use umbra_math::Point2i;
use umbra_render::{Acceptance, Decomposition, Film, PixelSum, WorkResult};
use umbra_scene::{FilmParameters, RenderJobOptions, SceneBuilder};

let mut builder = SceneBuilder::new();
builder.film(FilmParameters { resolution: Point2i::new(48, 32) });
let scene = builder.build()?;
let options = RenderJobOptions::default();

// What a Render Device would send back: one Work Result per Work Unit.
let decomposition = Decomposition::new(scene.full_resolution(), &options)?;
let mut film = Film::new(&scene, &options)?;
for unit in decomposition.work_units() {
    let area = decomposition.tile_bounds(unit.tile).area();
    let result = WorkResult::new(unit, vec![PixelSum::default(); area]);
    assert_eq!(film.accept(&result), Acceptance::Merged);
}
assert!(film.is_complete());
# Ok::<(), Box<dyn std::error::Error>>(())
```

## What is in it

| Module          | Contents                                                                                                                                                                                |
| --------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `decomposition` | `Decomposition`: the Tiles and Passes of a Render Job, its `WorkUnit`s (`TileIndex`, `PassIndex`) Pass by Pass in raster order, each Tile's `TileBounds` and each Pass's sample indices |
| `error`         | `Error`: why a Render Job cannot be cut into Work Units                                                                                                                                 |
| `rendered`      | `Rendered`, how a render of a `WorkUnit` ended: `Done` with its Work Result, or `Cancelled`; `Cancellation`, the request that stops it                                                  |
| `work_result`   | `WorkResult`: one `PixelSum` per pixel of a Tile, the `f64` sums of filter-weighted sensor RGB and of filter weights                                                                    |
| `film`          | `Film`, which accepts each Work Result as `Acceptance::Merged` or `Acceptance::Duplicate`, its `FilmHash`, and `finish` into an `Image`                                                 |
| `image`         | `Image`, written in the format its path's extension names                                                                                                                               |
| `atomic_file`   | `write_atomically`: a file written whole or not at all, for the image                                                                                                                   |
| `reproduction`  | `ReproductionRecord`, its versioned JSON and its text, and the `BuildIdentity` it holds                                                                                                 |

Not here yet: the Anomalous Pixel Samples of a Work Result, with
[#85](https://github.com/sergioffpc/umbra/issues/85).

## Design decisions

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
Job Report on stdout, not in the file.

### Finishing as pbrt-v4 does

`Film::finish` narrows each pixel's `f64` sums to `f32`, divides by the
weight when it is not zero, and converts sensor RGB to sRGB with the
Scene's matrix (pbrt-v4 `RGBFilm::GetPixelRGB`). It panics before the last
Work Unit is merged: an image exists only once every Pass is (ADR 0004).

## Things to know when using it

- **`Cancellation` is shared** between whoever cancels and every render it
  stops; each Render Device's renderer decides how often it checks it.
- **`Film::accept` panics** on a Work Result for a Work Unit outside the
  Render Job, or with the wrong number of pixels: either is a bug.
- **`write_atomically` leaves the target as it was on failure**, and syncs
  both the file and its directory.
- **A Work Result's pixels are row by row across its Tile**, and the Tiles
  at the right and bottom edges are cut short by the image.

## Tests

`tests/umbra_render/` tests the crate at its seam (spec #72):

- `film.rs`, the Film's acceptance: single-Pass Work Results merged in
  several orders give the same Film and hash; a duplicate with other sums
  leaves the Film unchanged; a Work Result lands on its Tile's pixels; the
  Passes of a Tile add up; a finished Film is each pixel's weighted mean in
  sRGB, against the BT.709 matrix.
- `image.rs`, the final image written in the format its path names.

The renderers' tests are in `umbra-cpu` and `umbra-gpu`; the EXR's bytes
and its Reproduction Record are tested through the binary, in
`crates/umbra/tests/render/`.

Run them with `cargo nextest run -p umbra-render`.
