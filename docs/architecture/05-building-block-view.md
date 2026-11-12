# 5. Building Block View

Umbra is a Cargo workspace of crates in `crates/<name>/` ([ADR 0015](../adr/0015-layered-workspace-with-a-seven-crate-no-std-core.md), [Code organisation and module boundaries](https://github.com/sergioffpc/umbra/issues/30)). The `#![no_std]` crates without `alloc` hold all per-Pixel-Sample code, which [ADR 0014](../adr/0014-gpu-ready-no-std-rendering-core.md) calls the core. The `std` crates around them build Scenes, run Render Jobs and distribute them. The core is compiled for `x86_64` and for `nvptx64-nvidia-cuda` ([ADR 0028](../adr/0028-one-core-for-cpu-and-gpu.md)): `umbra-kernels` runs it on the GPU, and `umbra-gpu` drives those kernels from the host.

## 5.1 Level 1: the workspace

```mermaid
graph LR
  subgraph core ["core: no_std, no alloc"]
    math[umbra-math] --> sampling[umbra-sampling] --> spectrum[umbra-spectrum]
    math --> geometry[umbra-geometry]
    sampling --> geometry
    geometry --> shading[umbra-shading]
    spectrum --> shading
    geometry --> camera[umbra-camera]
    spectrum --> camera
    shading --> integrators[umbra-integrators]
    camera --> integrators
  end
  integrators --> scene[umbra-scene]
  scene --> parser[umbra-parser]
  scene --> render[umbra-render]
  integrators --> render
  integrators --> kernels["umbra-kernels (nvptx64)"]
  kernels -. "PTX, built by build.rs" .-> gpu[umbra-gpu]
  scene --> gpu
  render --> gpu
  render --> cluster[umbra-cluster]
  gpu --> cluster
  cluster --> umbra["umbra (bin)"]
  render --> umbra
  parser --> umbra
```

Arrows point from a dependency to the crate that uses it; the dotted arrow is a build-time dependency, not a Cargo one. Only the dependencies between Umbra crates are drawn: each crate may also use any crate below it.

| Crate               | Responsibility                                                                                                                                                                                            | External dependencies        |
| ------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------- |
| `umbra-math`        | Vectors, points, normals, frames, transforms, `Interval` and floating-point error bounds, `libm` wrappers; an `x86_64` and an `nvptx64` backend                                                           | `libm`                       |
| `umbra-sampling`    | Sampling functions (warps), hashing and RNG, low-discrepancy sequences, `Sampler` and `SamplerKind`                                                                                                       | —                            |
| `umbra-spectrum`    | `Spectrum` and `SpectrumKind`, `SampledSpectrum`, `SampledWavelengths`, RGB and color spaces                                                                                                              | —                            |
| `umbra-geometry`    | `Ray`, `Bounds`, interactions (carrying `MaterialId`, `LightId`, `MediumId`), Shapes and `ShapeKind`, BVH node layout and traversal                                                                       | —                            |
| `umbra-shading`     | Textures, Materials, BxDFs, Lights and light samplers, Media, each with its `*Kind`                                                                                                                       | —                            |
| `umbra-camera`      | Cameras and `CameraKind`, Filters, sensor RGB, camera sample generation                                                                                                                                   | —                            |
| `umbra-integrators` | `SceneView`, the per-Pixel-Sample body of each integrator, the `Recorder` trait                                                                                                                           | —                            |
| `umbra-kernels`     | `#![no_std]`, built for `nvptx64` only and empty elsewhere: the wavefront kernels, OptiX entry points and OptiX device bindings in inline PTX, the trapping panic handler                                 | —                            |
| `umbra-scene`       | Owning `Scene` (arenas), `SceneBuilder`, deterministic BVH construction, `RenderJobOptions`                                                                                                               | `serde`                      |
| `umbra-parser`      | The `.pbrt` adapter: tokenising, includes, assets, strict validation, content hash                                                                                                                        | `nom`                        |
| `umbra-render`      | Renders Work Units into Work Results in one process; Film and its merge order; Pixel Traces; writing the final image                                                                                      | `rayon`, `image`             |
| `umbra-gpu`         | The GPU Worker's renderer: `cudarc`, the OptiX host side (acceleration structures, pipeline, shader binding table), the `SceneView` upload, the wavefront queues; embeds the PTX of `umbra-kernels`       | `cudarc`, `libloading`       |
| `umbra-cluster`     | Coordinator and Worker: wire protocol, QUIC, heartbeats, scheduling, reassignment, tail, Local Workers (CPU in-process, GPU in a child process), Build Identity admission, status line, Render Job Report | `tokio`, `quinn`, `postcard` |
| `umbra`             | The binary: `clap` subcommands `render` (`--render-devices`), `worker` (`--render-device`), `trace`, `bench`; `tracing` setup; the statistics of `bench`                                                  | `clap`, `tracing`            |

The final image is an EXR written by `umbra-render` with the `image` crate; the Reproduction Record is in the Render Job Report (§8.9, §8.15, [ADR 0020](../adr/0020-bit-identical-exr-final-image.md)).

## 5.2 Level 2: the core

The core crates follow pbrt-v4's dependency order. pbrt-v4's cycles are broken by IDs. An interaction in `umbra-geometry` names its Material, area Light and Medium by `u32` ID ([ADR 0008](../adr/0008-index-handle-arenas-and-allocation-free-rendering.md)), and turning it into a BSDF happens in `umbra-shading`. Camera and shading are parallel branches.

Inside a core crate, each pbrt-v4 chapter or section is a module, for example `umbra_shading::bxdfs`. Interfaces keep pbrt-v4's names and are dispatched through closed `*Kind` enums ([ADR 0006](../adr/0006-closed-enum-dispatch-for-rendering-interfaces.md)).

| Crate               | pbrt-v4 chapters | Question it answers                                               |
| ------------------- | ---------------- | ----------------------------------------------------------------- |
| `umbra-math`        | 3, 6.8, B.2      | How to operate on numbers and geometric quantities                |
| `umbra-sampling`    | 2, 8, A          | How to choose numbers                                             |
| `umbra-spectrum`    | 4.5–4.6          | How light is represented                                          |
| `umbra-geometry`    | 6–7              | Where a ray hits                                                  |
| `umbra-shading`     | 9–12             | What light does at that point                                     |
| `umbra-camera`      | 5, 8.8           | How a Pixel Sample becomes a ray, and radiance becomes sensor RGB |
| `umbra-integrators` | 13–15            | How one Pixel Sample is computed from start to end                |

Per-sample code receives a `SceneView<'_>` of borrowed arena slices, never `&Scene` (ADR 0014). Arena types are `#[repr(C)]` and Scene-resident `*Kind` enums `#[repr(C, u32)]`, so the host copies the slices into GPU buffers byte for byte, and every core function works on `Copy` values, never on a queue, a buffer or a thread index (§8.12, ADR 0028). `umbra-integrators` holds the steps of each integrator and the CPU loop over them; the GPU's wavefront over the same steps lives in `umbra-kernels`.

Every core crate has a `serde` feature, off by default, that derives `Serialize` and `Deserialize` for its Scene arena types with `serde`'s `default-features = false`. `umbra-scene` turns it on. The core crates build with the feature off in `cargo build` and on in CI's `--all-features` runs.

## 5.3 Level 2: interfaces outside the core

The signatures below are the contracts between crates. Error types and option structs are sketched, not final.

### `umbra-parser`

```rust
/// Reads a Scene File with its includes and assets, validates it strictly, applies the overrides, builds the Scene.
pub fn parse(scene_file: &Path, overrides: &Overrides) -> Result<Parsed, Error>;
pub struct Overrides { pub spp: Option<NonZeroU32>, pub seed: Option<Seed>, pub outfile: Option<PathBuf> }
pub struct Parsed { pub scene: Scene, pub options: RenderJobOptions, pub output: PathBuf, pub content_hash: ContentHash, pub warnings: Vec<Diagnostic> }
```

`parse` is the only public function. `Error::Invalid` lists every `Diagnostic` with its file, line and column, and nothing is built when there is one ([ADR 0003](../adr/0003-pbrt-v4-scene-files-parsed-on-the-coordinator.md)). It takes pbrt-v4's overrides because `--outfile` decides whether the output is an EXR, which is a `Diagnostic` (§8.15). The content hash covers the Scene File, its includes and its assets, for the Reproduction Record (§8.9).

### `umbra-scene`

```rust
impl SceneBuilder {
    pub fn add_shape(&mut self, shape: ShapeKind) -> ShapeId;   // one add_* per arena
    pub fn build(self) -> Result<Scene, Error>;           // builds the BVH
}
impl Scene { pub fn view(&self) -> SceneView<'_>; }
```

`SceneBuilder` is the seam that makes a Scene independent of its file format. The parser is one adapter, and tests that build Scenes directly are the other. `build` constructs the BVH deterministically ([ADR 0004](../adr/0004-deterministic-film-for-a-fixed-render-job.md)) and checks that its depth fits the fixed traversal stack. Replication sends the builder's arenas, without the BVH, and each Worker calls `build` itself. `RenderJobOptions` lives here because render, cluster and the binary all need it. The `.pbrt` directives fill it, and the binary applies command-line overrides on top.

### `umbra-render`

```rust
impl<'s> Renderer<'s> {
    pub fn new(scene: &'s Scene, options: &RenderJobOptions) -> Result<Self, Error>;
    pub fn render(&self, unit: WorkUnit, cancel: &Cancellation) -> Rendered;   // Done(WorkResult) | Cancelled
    pub fn trace(&self, request: &TraceRequest) -> Vec<PixelTrace>;
}
impl Film {
    pub fn new(scene: &Scene, options: &RenderJobOptions) -> Result<Self, Error>;
    pub fn accept(&mut self, result: &WorkResult) -> Acceptance;  // Merged | Duplicate
    pub fn finish(self) -> Image;
}
```

- `Renderer` hides everything between a Work Unit and its Work Result: the `dyn Integrator`, per-thread buffers, `par_iter` over rows, Anomalous Pixel Sample collection and the choice of `Recorder`. `render` may be called concurrently for several Work Units (§8.8). Cancellation is cooperative, checked between rows, and serves the speculative tail.
- `trace` re-runs Pixel Samples through the same code path as `render`, with a `TraceRecorder`. That shared path is what the bit-identical oracle tests (§8.9).
- `Film` merges each Work Result as it arrives, in any order (ADR 0022). A second Work Result for the same Work Unit is a `Duplicate` and is discarded unread. The scheduler decides what to assign, never how results are merged.

### `umbra-cluster`

```rust
pub struct RenderJob { pub scene: Scene, pub options: RenderJobOptions, pub record: ReproductionRecord }

pub fn coordinate(job: RenderJob, options: &CoordinatorOptions, status: &mut dyn io::Write)
    -> Result<RenderJobOutcome, Error>;   // { image: Image, report: RenderJobReport }
pub fn work(options: &WorkerOptions) -> Result<(), Error>;
```

- Both functions are synchronous. Each owns its `tokio` runtime, so callers never see `async`, QUIC, discovery, heartbeats or scheduling.
- `work` runs until the Worker is stopped, joining the Render Jobs of its Cluster one after another (§8.13). `WorkerOptions` carries the Cluster Name, the Render Device, the thread count and the Build Identity. A GPU Worker renders through `umbra-gpu` instead of `umbra-render`'s `Renderer`, behind the same seam.
- `umbra-cluster` receives a parsed Scene and does not depend on `umbra-parser`.
- `CoordinatorOptions` carries the listen address, the heartbeat and death timeouts, the Render Devices to start Local Workers on (CPU and every usable GPU by default), `Discovery::{Announce(ClusterName), Off}` and the Build Identity to admit. A Coordinator with `Discovery::Off` announces nothing and renders with its Local Workers alone.
- A distributed `render` and a single-Worker `bench` both go through `coordinate`, with different options.
- The status line goes to the `io::Write` it is given (§8.10). The Render Job Report is returned, not written, and the caller decides where it goes.

### `umbra` (binary)

The binary has `clap` derive subcommands: `render`, `worker`, `trace` and `bench`.

- **Parsing.** The binary parses the Scene File, applies command-line overrides and builds the Reproduction Record. It then calls `umbra-cluster`, or `umbra-render` for `trace`.
- **`bench`.** Its logic is modules of the binary: the confidence intervals and the comparison of Render Job Reports.

## 5.4 Workspace conventions

| Concern     | Rule                                                                                                                                                                                                                                                                                    |
| ----------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Layout      | Crates in `crates/<name>/`. The root `Cargo.toml` is a `[workspace]` with `members = ["crates/*"]`, `resolver = "3"` and the build profiles.                                                                                                                                            |
| Inheritance | `[workspace.package]` (version, edition, license, repository, authors), `[workspace.lints]` (every lint in force) and `[workspace.dependencies]`. Each crate sets `lints.workspace = true`.                                                                                             |
| Versioning  | One workspace version and `publish = false` everywhere. The Build Identity is the commit ([ADR 0012](../adr/0012-per-cpu-binaries-locked-to-one-commit.md)).                                                                                                                            |
| Core crates | `#![no_std]`, no `extern crate alloc`; every dependency builds without `std`. CI checks `std` by building `umbra-kernels` for `nvptx64`, which has none ([ADR 0028](../adr/0028-one-core-for-cpu-and-gpu.md)); `alloc` is kept out by review. No `f64` outside `umbra-math`'s backends. |
| Tests       | Unit tests in each crate. χ², `pdf`, normalisation and reciprocity tests (§8.11) live in the crate that owns the sampling routine. Cross-crate tests (the trace oracle, the counting allocator) live in `crates/umbra/tests/`.                                                          |
| Benchmarks  | `divan` micro-benchmarks in each crate's `benches/`. End-to-end measurement is `umbra bench` (§8.4).                                                                                                                                                                                    |
