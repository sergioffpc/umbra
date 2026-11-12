# A bit-identical EXR final image carrying its Reproduction Record

> **Amended by [ADR 0022](0022-determinism-on-one-machine-statistical-equality-across-machines.md).** The file is a pure function of the Film, but the Film is bit-identical across renders only when each Tile has one Pass, so the file-hash test renders a single Pass.
>
> **Amended by [ADR 0031](0031-renders-checked-by-eye.md).** Nothing computes relMSE against a Reference Image, and the `exr` crate reads images for `umbra trace` only. The whole-file test compares two renders made in one test run, so a change to the `exr` crate's compressor has no committed hash to update.
>
> **Amended on 2026-11-12, decided with the owner.** The image is written through the `image` crate, which also writes other formats, instead of `exr` directly. `image` writes no custom attributes, no half floats and no chosen compression, so: the Reproduction Record is in the Render Job Report only, as `reproduction_record`, and `umbra trace` reads it there; the header carries no pbrt-v4 `RGBFilm` metadata; pixels are 32-bit floats, and `savefp16` stays refused; the compression is `exr`'s default (RLE in 64 × 64 tiles). `image` takes `exr` without its `rayon` feature, so the file is written in one thread and its bytes are still a pure function of the Film. There is no `dyn` image-writer seam: `Image::write` picks the format from the path's extension, as `image` maps it. Scene validation still accepts only `.exr`. The rest of this ADR still applies.

The final image of a Render Job is an OpenEXR file whose bytes are a pure function of the Render Job, and it carries its Reproduction Record as one header attribute. Decided in [Image output](https://github.com/sergioffpc/umbra/issues/65).

- **EXR only.** The image format follows the extension of pbrt-v4's Film `filename`, or of the `--outfile` override, as in pbrt-v4. Only `.exr` is accepted. `.png`, `.pfm` and `.qoi` are rejected by Scene validation, before any Work Unit ([ADR 0003](0003-pbrt-v4-scene-files-parsed-on-the-coordinator.md)). Other formats join a later milestone only if a Reference Scene needs them, and only with somewhere to keep the Reproduction Record.
- **The `"rgb"` Film only in v0.1.** Every `RGBFilm` parameter is supported, including the named camera sensors. `"gbuffer"` and `"spectral"` are rejected as an invalid Scene File and join their family's milestone.
- **Reproduction Record as one attribute.** The header has one text attribute, `umbra.reproductionRecord`, holding versioned JSON (`"version": 1`). It is the serialisation of the same `serde` struct that `umbra trace` reads ([ADR 0011](0011-pixel-traces-by-deterministic-re-execution.md)). The Render Job Report carries an identical copy, so `umbra trace` also works from the report when the image lost its metadata. The output path is not part of the Reproduction Record, because it changes no Pixel Sample.
- **pbrt-v4 metadata without the clock.** The header carries the attributes pbrt-v4's `RGBFilm` writes: `worldToCamera`, `worldToNDC`, `pixelBounds` (data window), `fullResolution` (display window), `samplesPerPixel`, and the colour space `chromaticities`. There is no `MSE`: Umbra has no `--mse-reference-image`, and relMSE against a Reference Image is computed by `umbra validate` ([arc42 §8.11](../architecture/08-crosscutting-concepts.md#811-correctness-checks)). `renderTimeSeconds` is left out: timing lives in the Render Job Report. `savefp16` is honoured.
- **Bit-identical file.** ZIP compression, `LineOrder::Increasing` and a fixed attribute order. A test hashes the whole file, not only the Film, so two renders of one Render Job compare with `cmp`. This extends [ADR 0004](0004-deterministic-film-for-a-fixed-render-job.md) from the Film to the file.
- **The `exr` crate.** It is pure Rust with no `unsafe`, so it fits `unsafe_code = "deny"` ([ADR 0007](0007-unsafe-code-as-an-aggressive-optimisation.md)) without an exception. It sits behind the `dyn` image-writer seam in `umbra-render` ([arc42 §8.5](../architecture/08-crosscutting-concepts.md#85-polymorphism)). The same crate reads images for `umbra trace` and `umbra validate`.
- **Written once, atomically, on the Coordinator.** The Coordinator writes the image on the operator's machine ([ADR 0018](0018-sessions-launched-from-the-operators-machine.md)), only after every Pass is merged. It writes a temporary file in the target directory, calls `fsync`, then renames it over the target. An existing file is overwritten without asking, as in pbrt-v4. Relative paths resolve as in pbrt-v4. A cancelled or failed Render Job writes no image, only its Render Job Report (`outcome: cancelled` or `failed`, [arc42 §8.10](../architecture/08-crosscutting-concepts.md#810-progress-and-telemetry)).

## Considered options

- **PNG next to EXR**: rejected for v0.1. It is useful for a quick look, but it is an 8-bit sRGB quantisation that validation cannot use, and any EXR viewer already shows the image.
- **PFM and QOI for parity with pbrt-v4**: rejected. Neither has room for metadata, so the Reproduction Record would need a sidecar, and no Reference Scene uses them.
- **One EXR attribute per Reproduction Record field**: rejected. Viewers would show each field on its own line, but there would be two parsers to keep in step, and no explicit version.
- **`renderTimeSeconds` for full pbrt-v4 parity**: rejected. One timing field would make every file differ, so only the Film would be comparable, not the file.
- **`openexr` (bindings to the C++ reference library)**: rejected. It needs a C++ toolchain on every host and complicates the per-CPU binaries ([ADR 0012](0012-per-cpu-binaries-locked-to-one-commit.md)).
- **`image`**: rejected. It uses `exr` underneath but does not expose custom attributes.
- **A partial image (`<name>.partial.exr`) holding the fully merged Passes on cancellation**: deferred. Pass-major scheduling would make it well defined, but `umbra trace` and validation on a partial image are new cases, and there is no live view to motivate it.
- **Refusing to overwrite without `--force`**: rejected. It breaks pbrt-v4 parity and gets in the way of benchmark loops.

## Consequences

- An EXR viewer (tev, `exrheader`) shows the Reproduction Record as one JSON string.
- Converting the image to another format loses the Reproduction Record. The Render Job Report next to it still has a copy.
- A change to the `exr` crate's compressor can change the file bytes without changing the Film. The file-hash test catches it, and the golden hashes are updated with the dependency.
- `--outfile` is a Render Job option that the Reproduction Record does not hold, unlike every other option ([arc42 §8.9](../architecture/08-crosscutting-concepts.md#89-per-pixel-debugging)).
