# 4. Solution Strategy

> Only the delivery roadmap (sections 4.1 and 4.2) is settled. The rest of the solution strategy is written from decisions already made, on the implementation-plan map.

## 4.1 Delivery roadmap

Umbra reaches all of pbrt-v4 on CPU through fifteen milestones ([ADR 0019](../adr/0019-milestones-defined-by-the-reference-scenes-they-unlock.md), [Milestone sequencing](https://github.com/sergioffpc/umbra/issues/31)).

- **Scope and order.** Each milestone is defined by the Reference Scenes from the pinned `mmp/pbrt-v4-scenes` that it makes fully renderable, and its scope is the smallest `.pbrt` subset they need. A feature no Reference Scene uses joins the milestone that builds its family and is validated on a Reference Scene Variant (§8.11).
- **Releases.** Each milestone is a GitHub Milestone and a release tagged `v0.N.0` on `main`.
- **Closing issues.** Every milestone closes with a benchmark measurement issue (§8.4) and a manual validation issue (§8.11). Both check every Reference Scene and variant unlocked so far against the targets of section 10.

```mermaid
flowchart LR
    M1["v0.1 First light"] --> M2["v0.2 Distribution"] --> M3["v0.3 Metals, glass, textures"] --> M4["v0.4 Environment lighting"] --> M5["v0.5 Measured materials"]
    M5 --> M6["v0.6 Production scenes"] --> M7["v0.7 Lights and lenses"] --> M8["v0.8 Homogeneous media"] --> M9["v0.9 Heterogeneous media"] --> M10["v0.10 Curves and hair"]
    M10 --> M11["v0.11 Subsurface"] --> M12["v0.12 Bidirectional"] --> M13["v0.13 Photon mapping"] --> M14["v0.14 Metropolis"] --> M15["v1.0 Parity"]
```

| Version | Name | Adds | Reference Scenes it unlocks | Validated through variants |
| --- | --- | --- | --- | --- |
| v0.1.0 | First light | `trianglemesh`, `sphere`, `loopsubdiv`; `diffuse`, `coateddiffuse`; diffuse area light; `bvh` light sampler; `halton`; `gaussian`; `rgb` film with the default sensor; `perspective`; `volpath` with media rejected. All the tooling the closing issues need (§4.2). | killeroo-simple | — |
| v0.2.0 | Distribution | Remote Workers: QUIC, wire protocol, heartbeats, reassignment, speculative tail, Build Identity admission. Accepted on QS-2, QS-7 and QS-8. | none (those of v0.1.0) | — |
| v0.3.0 | Metals, glass and textures | `conductor`, `dielectric`; named spectra and `.spd` files; `disk`; `distant` light; `imagemap`, `scale`, `constant` textures (PNG); bump mapping; spectral camera sensors; `ActiveTransform` motion blur; `sobol`, `pmj02bn` | killeroo-moving, killeroo-gold, pbrt-book, lte-orb-silver, lte-orb-rough-glass | `path`, `simplepath`, `randomwalk`, `ambientocclusion`; `box`, `triangle`, `mitchell`, `lanczos`; `independent`, `stratified`, `paddedsobol`; `TransformTimes` |
| v0.4.0 | Environment lighting | `coatedconductor`, `mix` material; image `infinite` light (EXR); gzipped PLY; object instancing | killeroo-coated-gold, ganesha, transparent-machines (5), bmw-m6 | PFM and TGA images |
| v0.5.0 | Measured materials | `measured`; `bilinearmesh`; `gbuffer` film | lte-orb-blue-agat-spec, lte-orb-simple-ball, sportscar (2) | `aov` |
| v0.6.0 | Production scenes | `diffusetransmission`, `thindielectric`; alpha cut-outs; normal maps; `mix`, `wrinkled`, `windy` textures; `zsobol` | sanmiguel, bistro, landscape, zero-day, villa-daylight, pavilion-day (those needing nothing from v0.7.0) | `checkerboard`, `dots`, `fbm`, `marble`, `bilerp`, `ptex`; `ColorSpace`, `Attribute`, `Option` |
| v0.7.0 | Lights and lenses | blackbody emission; `point`, `spot`, uniform `infinite` lights; thin-lens depth of field; `realistic` camera | the rest of the v0.6.0 group, villa-lights-on, sanmiguel-realistic-courtyard | `orthographic`, `spherical`; `goniometric`, `projection`, image area lights; `spectral` film |
| v0.8.0 | Homogeneous media | `homogeneous` medium, `MediumInterface`; `directionmix` texture; infinite-light portals; displacement on PLY meshes | dambreak (2), crown, kroken (7), watercolor (18) | `simplevolpath` |
| v0.9.0 | Heterogeneous media | `uniformgrid`, `cloud`, `nanovdb` media, emissive grids | smoke-plume, clouds, disney-cloud, bunny-cloud, explosion | `rgbgrid` |
| v0.10.0 | Curves and hair | `curve` (cylinder and flat), `cylinder`; hair BxDF | hair (11), bunny-fur | — |
| v0.11.0 | Subsurface | `subsurface` material and BSSRDF | sssdragon (3), head | — |
| v0.12.0 | Bidirectional | `bdpt` | pavilion-night | `lightpath` |
| v0.13.0 | Photon mapping | `sppm` | contemporary-bathroom | — |
| v0.14.0 | Metropolis | `mlt` | — | `mlt` on Reference Scenes |
| v1.0.0 | Parity | No feature. The per-architecture tuning backlog (§8.13) until QS-5 reaches ≥ 1.0 on each machine with no Reference Scene below 0.8, then validation of every Reference Scene and variant again. | — | — |

Scene names are paths in `mmp/pbrt-v4-scenes` without their folder; a count in parentheses is the number of variants of that scene in the collection. When a milestone is detailed, a Reference Scene may move to the next milestone if it needs a feature that milestone delivers. Milestones after the first stay coarse until they are planned.

## 4.2 The first milestone

`v0.1.0 — First light` renders `killeroos/killeroo-simple` on one machine. It goes through `coordinate` with the in-process local Worker and through the Film's Pass-order merge, so QS-2 holds locally from the start. It is split into these issues, in crate dependency order. Issues 2 to 8 can run in parallel as far as the graph of §5.1 allows.

| # | Issue | Contents |
| --- | --- | --- |
| 1 | Workspace and CI | The twelve crates of §5.1 and the conventions of §5.4. CI: `aarch64-unknown-none` build of the core, clippy, rustfmt, cargo-deny, typos, the book-citation check and `docs/book-map.md` (QS-4), the counting-allocator test. |
| 2 | `umbra-math` | Vectors, points, normals, transforms, `Interval`, floating-point error bounds, `libm` wrappers. pbrt-v4 tests ported. |
| 3 | `umbra-sampling` | Hashing and RNG, warps, `HaltonSampler` with its permutations and scrambling, `SamplerKind`. Sampling tests. |
| 4 | `umbra-spectrum` | `SampledSpectrum`, `SampledWavelengths`, CIE matching functions, the sRGB `RGBColorSpace` and its RGB-to-spectrum tables. |
| 5 | `umbra-geometry` | `Ray`, `Bounds`, interactions, triangle and sphere intersection, BVH node layout and traversal. |
| 6 | `umbra-shading` | `DiffuseBxDF`, `CoatedDiffuseBxDF` (LayeredBxDF), `DiffuseAreaLight`, `BVHLightSampler`. χ², `pdf`, normalisation and reciprocity tests. |
| 7 | `umbra-camera` | `PerspectiveCamera`, `GaussianFilter`, the default `PixelSensor`, sensor RGB. |
| 8 | `umbra-integrators` | `SceneView`, `VolPathIntegrator` without media, `Recorder` and `NoRecorder`. |
| 9 | `umbra-scene` | Arenas, `SceneBuilder`, Loop subdivision refined to triangles at Scene build, deterministic BVH construction, `RenderJobOptions`. |
| 10 | `umbra-parser` | `nom` tokenizer, includes, the v0.1.0 subset, strict validation with every error located, content hash. |
| 11 | `umbra-render`: Renderer and Film | `Renderer::render`, `Film::accept` with the Pass-order merge, Anomalous Pixel Samples. |
| 12 | `umbra-render`: image writer | The final image and its Reproduction Record. Waits on the image output decision. |
| 13 | `umbra-render`: Pixel Traces | `TraceRecorder`, `Renderer::trace`, glTF export with display tessellation of spheres and meshes, the bit-identical oracle test. |
| 14 | `umbra-cluster`, local only | `coordinate` with the local Worker, the status line, the Render Job Report. |
| 15 | `umbra` binary | `render`, `trace`, `bench` and `validate`. |
| 16 | pbrt-v4 reference script | Builds the pinned pbrt-v4, renders 16 seeds and the Reference Image, publishes the release `pbrt-v4-reference-<commit>`. The killeroo-simple entry, with its benchmark and validation settings. |
| 17 | Golden Film hashes | CI renders on arm64 and x86-64 and caches the downloaded Scenes. |
| 18 | Benchmark measurement v0.1.0 | Sets the baseline on the three machines (no regression check). QS-5 ≥ 0.5, QS-6. |
| 19 | Validation v0.1.0 | `umbra validate` on killeroo-simple, QS-1, QS-2 on one machine. |
