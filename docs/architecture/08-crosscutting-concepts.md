# 8. Crosscutting Concepts

## 8.1 Numerics and spectral representation

Umbra follows pbrt-v4's numeric model ([ADR 0001](../adr/0001-numeric-model.md)) and requires bit-identical results on every Worker architecture ([ADR 0002](../adr/0002-bit-identical-results-across-architectures.md)).

| Concern | Rule |
| --- | --- |
| Precision | `f32` throughout the rendering core; no `f64` option. `f64` only for the per-pixel sums in Work Results and the Film. |
| Spectrum | Spectral only. `SampledSpectrum` / `SampledWavelengths` with a constant `N_SPECTRUM_SAMPLES = 4`, backed by `Vec4`. Visible-wavelength sampling as in pbrt-v4. |
| Ray robustness | pbrt-v4 error bounds: `Interval`, `Point3fi`, a `pError` for every shape, `offset_ray_origin`, `spawn_ray_to`, `1 + 2γ(3)` in ray-box tests (also in SIMD). |
| RGB input | Jakob–Hanika RGB-to-spectrum tables generated in `build.rs` with `libm`; sRGB first, more colour spaces when a milestone needs them. Hash checked in CI. |
| Spectral data | pbrt-v4's CIE matching functions and named spectra as Rust `const` arrays (Apache-2.0, attributed in `NOTICE`). |
| Film | Pixel Samples are converted to sensor RGB on the Worker. Work Results carry `f64` weighted-RGB and weight sums per pixel. The Film sums in `f64`. Filter importance sampling only, so no cross-pixel splatting. |
| Math | A core `math` module ports pbrt-v4's helpers (`difference_of_products`, `sum_of_products`, `evaluate_polynomial`, compensated sums, `safe_sqrt`, `safe_asin`, `erf_inv`, …). Explicit `mul_add` where pbrt uses FMA, with hardware FMA required. Transcendentals from `libm`. No fast-math. |
| SIMD | No approximate `rcp`/`rsqrt`. Kernels are bit-identical to their scalar reference, which is the test oracle. Explicit NaN semantics for `min`/`max`. |
| Floating-point state | IEEE defaults; never flush-to-zero or denormals-are-zero. |
| NaN / Inf | `debug_assert!` wherever pbrt-v4 has a `DCHECK`. |

The flow of numeric precision for one Pixel Sample:

```mermaid
flowchart LR
    W["SampledWavelengths<br/>4 × f32"] --> L["Radiance<br/>SampledSpectrum (f32)"]
    L --> S["Sensor RGB<br/>f32"]
    S --> R["Work Result<br/>per-pixel sums (f64)"]
    R --> F["Film on the Coordinator<br/>per-pixel sums (f64)"]
```

## 8.2 Scene input

Umbra reads pbrt-v4 Scene Files only, validates them strictly and parses them once on the Coordinator ([ADR 0003](../adr/0003-pbrt-v4-scene-files-parsed-on-the-coordinator.md)).

| Concern | Rule |
| --- | --- |
| Format | pbrt-v4 `.pbrt`, unextended, at a pinned `mmp/pbrt-v4` commit. Each milestone declares the supported subset. No pbrt-v3, no custom format. |
| Validation | Any unsupported directive, type or parameter rejects the Render Job before any Work Unit is assigned. All uses are reported, each with `file:line:column`. Recognised but unused parameters produce a warning. |
| Parser | `nom` + `nom_locate` for the lexical layer and directives, behind a `ParserTarget`-style interface. Graphics state and Scene builder follow pbrt-v4. |
| Replication | The Coordinator builds a format-independent Scene, with assets referenced by content hash, and replicates it to every Worker. Workers build their own acceleration structures. |
| Render Job options | Tile size, Pixel Samples per Pass and Workers are given at submission, never in the Scene File. pbrt-v4-style overrides (`--spp`, `--seed`, …) take precedence and are applied when the Scene is built. |

```mermaid
flowchart LR
    F["Scene File<br/>.pbrt + includes + assets"] --> P["Parser<br/>(Coordinator)"]
    O["Render Job options<br/>overrides"] --> B
    P --> B["Scene builder<br/>(Coordinator)"]
    B --> S["Scene"]
    S --> W1["Worker: replica + BVH"]
    S --> W2["Worker: replica + BVH"]
```

## 8.3 Determinism

For a fixed Scene, seed and Render Job options, the final Film is bit-identical regardless of Workers, threads, arrival order or failures ([ADR 0004](../adr/0004-deterministic-film-for-a-fixed-render-job.md)).

| Concern | Rule |
| --- | --- |
| Scope | Fixed: Scene, seed, Tile size, Pixel Samples per Pass. Free: number of Workers, Worker and thread per Work Unit, arrival order, failures and reassignments. Changing the decomposition may change the last bit. |
| Sampling | A Pixel Sample is a pure function of `(Scene, seed, pixel, sample index)`. pbrt-v4 samplers, hashes (`MixBits`, `MurmurHash64A`), PCG32 and Sobol tables are ported faithfully. No randomness outside the sampler on the render path, enforced by lint. Default seed 0, `--seed` overrides it. |
| Work Result order | Each pixel sums its Pixel Samples in ascending sample index. Threads split a Work Unit by pixel. |
| Film order | Per Tile, Work Results are merged in Pass order. Early arrivals wait on the Coordinator. |
| Splats | Cross-Tile contributions go into a separate splat accumulator, merged in the total order (Pass, Tile index). MLT and SPPM designs must preserve the guarantee. |
| BVH | A pure function of the Scene: stable SAH partitioning, explicit tie-breaks, parallelism only over disjoint subtrees joined in a fixed order. |
| Termination | Every Render Job renders all its Passes. No time budget, no adaptive sampling. Cancelling produces no image. |
| Verification | Film-hash tests in CI on arm64 and x86-64. Invariance tests (thread count, Work Unit order, simulated Worker failure). A 64-bit hash per Work Result, compared when duplicates arrive. An opt-in `verify` Render Job option. |

Merge order for one Tile on the Coordinator:

```mermaid
sequenceDiagram
    participant W as Workers
    participant C as Coordinator
    participant F as Film
    W->>C: Work Result (Tile 7, Pass 2)
    Note over C: Pass 1 not merged yet: hold
    W->>C: Work Result (Tile 7, Pass 1)
    C->>F: merge Pass 1
    C->>F: merge Pass 2 (held)
```

## 8.4 Performance measurement

How performance claims are measured and guarded ([Benchmark and profiling infrastructure](https://github.com/sergioffpc/umbra/issues/14)). It backs the rule that an aggressive optimisation needs a benchmark showing the gain.

| Concern | Rule |
| --- | --- |
| Proof of a gain | An optimisation is accepted only on an end-to-end gain: one Worker, no network, on the Reference Scenes. Micro-benchmarks help to iterate and to locate a gain, but never justify one on their own. |
| Metrics | The main metric is Pixel Samples per second in the render phase (wall-clock, all threads of the Worker). Each other phase is timed on its own: parse, Scene build, BVH build. They are the comparable fields of the Render Job Report written at the end of every Render Job (§8.10). |
| Reference Scenes | Taken only from `mmp/pbrt-v4-scenes` at a pinned commit (§8.11), downloaded and never vendored. Each milestone adds the ones it can render, and none is ever removed. Each entry fixes resolution, Pixel Samples per pixel, seed and Tile size for benchmarking, sized for 10–60 s per machine, and separately for validation (§8.11). |
| Where | Measured by hand on the three target machines (§7.1) with `umbra bench`, and the report is attached to the PR. Arm64 (M2) is required for NEON changes, at least one x86-64 machine for AVX2 changes, and the R630 for any threading or memory change (NUMA). Shared CI runners only smoke-test: `cargo bench --no-run` and one small Scene rendered to completion, with no numbers compared. |
| No remote triggers | No self-hosted runners and no GitHub-triggered execution on the target machines. Every measurement on them is started by hand. |
| Noise | Baseline and candidate commits run interleaved (A, B, A, B, …) on the same idle machine: 1 discarded warm-up, then 5 runs each. A gain counts if the 95 % confidence interval of the difference excludes zero and the median gain is ≥ 2 %. The report records the CPU model, OS, commit, build flags, and the governor and frequency when available. |
| Regressions | Every milestone ends with a benchmark measurement issue, run by hand on all three machines against the previous milestone. `umbra bench` reads the Render Job Report of every run and commits the aggregated result to `docs/benchmarks/<milestone>/<machine>.json`. A regression above 2 % in a comparable field blocks closing the milestone until it is fixed or explained. Informative fields are carried along (median of the runs) to explain a difference and never block. `umbra bench` refuses to compare reports of different schema major versions. |
| Distribution | From the milestone that introduces distribution, the measurement issue also records three numbers. Scaling efficiency is the cluster's Pixel Samples per second divided by the sum of each Worker's standalone rate. The other two are Scene replication time and Worker idle time (no Work Unit). A drop of more than 2 percentage points in efficiency, or an efficiency below the target of section 10 (QS-7), blocks the milestone. |
| pbrt-v4 reference | The measurement issue also renders each Reference Scene with pbrt-v4 (CPU, pinned commit) on the same machine, at the same resolution, Pixel Samples per pixel and thread count. The Umbra / pbrt-v4 speed ratio is recorded on every machine and blocks the milestone only below the targets of section 10 (QS-5: geometric mean ≥ 0.5 each milestone, ≥ 1.0 at the last). The same renders feed the correctness target QS-1. |
| Micro-benchmarks | `divan` in `benches/`. A readable reference and its optimised version are benchmarked side by side in one generic bench, and `AllocProfiler` checks the allocation-free core. |
| Instrumentation | Coarse `tracing` spans only: Render Job, each phase, Work Unit, Work Result send and merge. Never inside the rendering core, never per Pixel Sample or per ray. Tracy via `tracing-tracy` behind the Cargo feature `tracy`, off by default. |
| Profilers | Sampling profilers on the `profiling` build profile: samply on all three machines, plus Instruments on macOS and `perf` (hardware counters) on Linux. |

What answers which question:

```mermaid
flowchart LR
    Q1["Where does the time go<br/>in a real render?"] --> T["tracing spans + Tracy"]
    Q2["Which function or instruction<br/>is hot?"] --> S["samply / Instruments / perf"]
    Q3["Is the optimised kernel faster<br/>than its reference?"] --> D["divan micro-benchmark"]
    Q4["Did the render get faster?"] --> B["umbra bench<br/>(the only proof)"]
```

## 8.5 Polymorphism

How pbrt-v4's interfaces are dispatched in Rust ([ADR 0006](../adr/0006-closed-enum-dispatch-for-rendering-interfaces.md)). The rule follows how often a call happens: closed enums on the per-ray and per-Pixel-Sample path, `dyn Trait` only at coarse seams.

| Concern | Rule |
| --- | --- |
| Hot-path interfaces | Shape, Primitive (acceleration structure included), Material, BxDF, Light, FloatTexture, SpectrumTexture, Medium, PhaseFunction, Sampler, Camera and Filter dispatch through a closed `enum` and `match`. Never `dyn`. |
| Shape of an interface | A trait with the book's name is the contract (`trait Shape`). Concrete types keep pbrt-v4's names (`Sphere`, `DiffuseBxDF`) and implement it. The enum is `<Interface>Kind` (`ShapeKind`) and implements the trait with a hand-written `match` per method. pbrt-v4's `LightType` keeps its name, distinct from `LightKind`. |
| Exhaustive matches | No `_` arm in a `match` on a `*Kind` enum. Enforced crate-wide by lint (§8.6). |
| Fixed per Render Job | Sampler, Camera and Filter are still matched per call. Hoisting the `match` into a monomorphised loop is an aggressive optimisation (benchmark, isolated contract, per-call version as reference). |
| Generics | Free when they are not interface dispatch (`Vector3<T>`, `Bounds3<T>`). |
| `dyn Trait` | Only for code called at most once per Work Unit or outside rendering: `dyn Integrator`, network transport, image writers, `ParserTarget`, telemetry. |
| BxDF lifetime | Returned by value as `BxDFKind` and held on the stack inside the `BSDF`. `size_of::<BxDFKind>() <= 256` is asserted at compile time. `MixMaterial` picks one material stochastically, so there is no mixed BxDF. |
| Textures | `FloatTextureKind` and `SpectrumTextureKind`. No `TextureEvaluator` until a wavefront driver needs it. |

Where each dispatch mechanism sits, by call frequency:

```mermaid
flowchart TD
    J["Render Job"] -->|"once per Work Unit: dyn Integrator"| I["Integrator<br/>(path, volpath, BDPT, …)"]
    I -->|"per Pixel Sample: SamplerKind, CameraKind, FilterKind"| P["Pixel Sample"]
    P -->|"per ray: PrimitiveKind → ShapeKind"| H["Intersection"]
    H -->|"per hit: MaterialKind, TextureKinds"| B["BSDF holding a BxDFKind by value"]
    H -->|"per light sample: LightKind"| L["Light sample"]
```

## 8.6 Unsafe code and lints

What the compiler and clippy enforce beyond style ([Unsafe policy and lint configuration](https://github.com/sergioffpc/umbra/issues/21)). The full lint set lives in `[lints]` in `Cargo.toml` and in `clippy.toml`. `CONTRIBUTING.md` explains it for contributors. Every warning is an error in CI.

| Concern | Rule |
| --- | --- |
| `unsafe` | Denied crate-wide. An exception is an aggressive optimisation (benchmark, safe contract, tested safe reference) opened with `#[expect(unsafe_code, reason)]` on the narrowest item ([ADR 0007](../adr/0007-unsafe-code-as-an-aggressive-optimisation.md)). No module is unsafe by default. |
| `unsafe` documentation | One unsafe operation per block, a `// SAFETY:` comment on every block, a `# Safety` section on every `unsafe fn`, and no `SAFETY` comments on safe code. |
| `unsafe` verification | Tests of every `unsafe` item run under Miri in CI (nightly pinned for that job), or under AddressSanitizer on Linux where Miri cannot execute the code. The job lands with the first exception. |
| Exhaustive dispatch | No `_` arm in a `match` on an enum (`wildcard_enum_match_arm`, `match_wildcard_for_single_variants`), so a new `*Kind` variant breaks the build wherever it is not handled (§8.5). On a `#[non_exhaustive]` foreign enum, a `_` arm is allowed after listing the known variants. |
| Bit-identical numerics | `disallowed-methods` bans `std`'s transcendental `f32`/`f64` methods (platform libm), approximate reciprocal intrinsics (`rcp`, `rsqrt`, `vrecpe`, `vrsqrte`, `wide`'s `recip`) and FP control-register writes (flush-to-zero, denormals-are-zero). Tests are not exempt ([ADR 0002](../adr/0002-bit-identical-results-across-architectures.md)). |
| Output | No `print!`/`eprint!` outside tests: output goes through `tracing` and the Render Job Report (§8.10). |
| Silencing | `#[expect(lint, reason = "...")]` on the narrowest item. `#[allow]` and reasonless attributes are rejected. |

## 8.7 Memory and allocation

How the Scene is stored and where rendering may allocate ([ADR 0008](../adr/0008-index-handle-arenas-and-allocation-free-rendering.md)). Objects live in per-type arenas and refer to one another by typed `u32` handles. Nothing on the per-Pixel-Sample path allocates.

| Concern | Rule |
| --- | --- |
| Scene storage | One `Vec<T>` arena per concrete type. References are typed `u32` handles (`MeshId`, `MaterialId`, `Handle<T>`), never `&T`, `Box`, `Arc` or raw `usize`. Validation rejects more than `u32::MAX` objects of one type. |
| `*Kind` size | Scene-resident `*Kind` enums are tag plus handle(s), with `size_of <= 16` asserted at compile time. `ShapeKind::Triangle { mesh, index }` is inline. `BxDFKind` is by value (≤ 256 B, §8.5). `SamplerKind`, `CameraKind` and `FilterKind` are by value. Inline payloads are an aggressive optimisation. |
| Scene lifetime | `SceneBuilder` mints handles and freezes into an immutable `Scene` (BVH built). Threads share `&Scene`, with no `Arc`, locks or global mutable state. Dropped at the end of the Render Job. |
| Per-thread memory | A per-thread context of typed buffers (BDPT/MLT vertices, …) sized at Render Job start and cleared between Pixel Samples. No `ScratchBuffer`. |
| Allocation boundary | Allowed per Render Job, per Work Unit (Work Result buffer) and outside rendering. Never per Pixel Sample, except while recording a Pixel Trace. A counting-allocator test (N vs 2N Pixel Samples) enforces it for every integrator. |
| Per-sample contract | `SceneView` (`get(handle)`, slices), `&mut` slices of the thread context and `Copy` values. No `alloc` dependency (§8.12). Bounds-checked indexing, `get_unchecked` only under §8.6. |
| Alignment | `CachePadded` (128 B) for per-thread mutable data adjacent to other threads'. Natural alignment elsewhere. A cache-line BVH node layout is an aggressive optimisation. |
| NUMA | One Worker per NUMA node gives each node its own Scene ([ADR 0017](../adr/0017-one-worker-per-numa-node.md)). No NUMA-local allocation inside a Worker. The Scene is pointer-free and `Clone`, so an in-process replica per node is a backlog entry (§8.14). |
| Global allocator | System allocator. Replacing it is an aggressive optimisation measured on the scene-build phase. |

Who allocates what, and when:

```mermaid
flowchart TD
    J["Render Job start"] -->|"allocates"| S["SceneBuilder → immutable Scene<br/>(per-type arenas, BVH)"]
    J -->|"allocates"| C["Per-thread contexts<br/>(typed buffers, CachePadded)"]
    W["Work Unit"] -->|"allocates or reuses"| R["Work Result buffer"]
    P["Pixel Sample"] -->|"reads SceneView, clears and fills"| C
    P -.->|"never allocates"| X["heap"]
```

## 8.8 Work scheduling

How a Render Job is split and scheduled across Workers and cores ([ADR 0009](../adr/0009-pull-based-pass-major-scheduling.md), [ADR 0010](../adr/0010-quic-transport-between-coordinator-and-workers.md)).

| Concern | Rule |
| --- | --- |
| Decomposition | Square Tiles, 32 × 32 by default; Passes of 64 Pixel Samples by default. Both are Render Job options, never derived from the cluster. |
| Assignment | Workers pull Work Units, about two in flight per eight threads. Pass-major order, raster Tile order within a Pass. Pass p of a Tile waits until Pass p − 2 of that Tile is merged. |
| Inside a Worker | One rayon pool over every logical CPU of the Worker's NUMA node. Work Units in flight run concurrently, each splitting its rows with `par_iter`. No thread pinning by default (§8.14). |
| Failure detection | Connection closed, `Fatal`, or 5 s without a heartbeat (sent every second). Both durations are Coordinator options. No per-Work-Unit timeout. A Worker declared dead by heartbeat has its connection closed with `WORKER_TIMED_OUT` (§8.13), so a dead Worker is always a closed connection. |
| Reassignment | A dead Worker's Work Units go to the front of the queue. A reconnecting Worker joins as new; `scripts/workers up` restarts a Worker Slot at most three consecutive times (ADR 0018). Nothing arrives from a dead Worker. The first Work Result per Work Unit is accepted; later ones (speculative copies, reassigned Work Units whose stream had already ended) are hash-compared and discarded. With no Workers, the Render Job waits and the status line shows `waiting: no Workers`. |
| Tail | Once the queue is empty, the oldest Work Unit in flight is copied to a free Worker, at most once. The first Work Result wins, the other is cancelled. |
| Transport | QUIC (`quinn`) to remote Workers; an in-memory transport to the Coordinator's optional local Worker. |

```mermaid
sequenceDiagram
    participant W1 as Worker A
    participant C as Coordinator
    participant W2 as Worker B
    W1->>C: request Work Units
    C->>W1: (Tile 0, Pass 0), (Tile 1, Pass 0)
    W2->>C: request Work Units
    C->>W2: (Tile 2, Pass 0), (Tile 3, Pass 0)
    W1->>C: Work Result (Tile 0, Pass 0)
    Note over C: queue empty, Tile 3 still in flight
    C->>W1: copy of (Tile 3, Pass 0)
    W1->>C: Work Result (Tile 3, Pass 0)
    C->>W2: cancel (Tile 3, Pass 0)
```

## 8.9 Per-pixel debugging

How to see how a pixel was computed ([ADR 0011](../adr/0011-pixel-traces-by-deterministic-re-execution.md), [Per-pixel debugging](https://github.com/sergioffpc/umbra/issues/24)). A Render Job never records a Pixel Trace. `umbra trace` re-runs the chosen Pixel Samples later, in one local process, with recording switched on.

| Concern | Rule |
| --- | --- |
| Production | `umbra trace final.exr --pixel x,y` re-runs Pixel Samples locally. It parses the Scene File and builds the BVH itself, with no Coordinator and no Workers. The protocol and Work Results carry no traces. |
| Addressing | pbrt-v4 raster convention: origin at the top left, full Film resolution even with a crop window. `--pixel x,y`, or `--pixels x0,x1,y0,y1` (a small rectangle, capped). `--sample i` selects one Pixel Sample; without it, every Pixel Sample of the pixel is traced. |
| Reproduction | The final image carries a Reproduction Record in its EXR header (`umbra.reproductionRecord`, versioned JSON, §8.15): the Scene File path, a content hash of the Scene File with its includes and assets, the seed, every Render Job option except the output path, and the Umbra commit. The Render Job Report carries a copy, and `umbra trace` accepts either. `umbra trace` refuses to run on a mismatch and names what differs. `--force` runs anyway, with a warning. |
| Detail levels | Path, the default: camera sample, sampled wavelengths, primary ray. Then, per vertex: hit point, normal, Shape, Material, BSDF, BSDF sample (`f`, `pdf`, flags), light sample (Light, selection probability, `Li`, `pdf`, shadow-ray outcome), MIS weights, throughput before and after, Russian roulette, and the contribution added to `L`. Then the termination reason and the sampler dimension consumed by each request. Traversal (`--detail traversal`) adds, per ray, the BVH nodes visited and the primitives tested with their hit distances. |
| Film side | Each Pixel Sample ends with the filter offset and weight, the sensor RGB and the value after `maxcomponentvalue` clamping. The trace ends with the pixel summary: the weighted sum of the traced Pixel Samples. |
| Mechanism | Integrators and traversal take `R: Recorder` (`no_std` core). `NoRecorder` is zero-sized and compiled away; `TraceRecorder` lives outside the core and may allocate (§8.7). Event arguments are values already computed. |
| Oracle | A test checks that radiance and the pixel summary are bit-identical with `NoRecorder` and `TraceRecorder`, and match the Work Result. |
| Text output | An indented tree per Pixel Sample on stdout, written through `io::Write` (not `print!`, §8.6). Each `f32` is printed in its shortest round-trip form, so two traces compare exactly with `diff`. No JSON. |
| 3D output | `--scene-export trace.glb` writes glTF that Blender opens natively. It contains the Scene as resolved by Umbra, tessellated for display only, together with the traced paths and the shadow rays, unoccluded and occluded in separate materials. At traversal level it also contains the wireframe boxes of the visited BVH nodes, one collection per ray, with hit and missed nodes in separate materials and the tested primitives highlighted. Scene and paths share one world space, so they always line up. |
| Anomalous Pixel Samples | A Pixel Sample with NaN, infinite or negative radiance is discarded, as in pbrt-v4. The Work Result lists the first 16 per Work Unit as (pixel, sample index, kind), plus a total count. At the end of the Render Job the Coordinator logs them in deterministic order, as ready-to-run `umbra trace` commands. |
| Integrators | Tracing is an acceptance criterion of every integrator. volpath adds medium events; BDPT adds both subpaths and each `(s, t)` strategy with its MIS weight. Splats that other Pixel Samples send to the inspected pixel do not appear in its trace. MLT and SPPM are decided with their work decomposition. |

From a suspicious pixel to its trace:

```mermaid
flowchart LR
    R["Render Job"] --> I["final.exr<br/>(with Reproduction Record)"]
    R --> G["Coordinator log:<br/>Anomalous Pixel Samples<br/>as umbra trace commands"]
    I --> V["image viewer<br/>(pixel coordinates)"]
    V --> T["umbra trace final.exr --pixel x,y"]
    G --> T
    T -->|"hash and version match"| E["re-run with TraceRecorder"]
    E --> X["text trace on stdout"]
    E --> B["trace.glb → Blender"]
```

## 8.10 Progress and telemetry

What a running Render Job shows and what it leaves behind ([Progress metrics](https://github.com/sergioffpc/umbra/issues/25)). There is no live view, so progress is text on the Coordinator's terminal, and the lasting record is the Render Job Report.

| Concern | Rule |
| --- | --- |
| Status line | On the Coordinator's stderr, redrawn in place when stderr is a TTY. One aggregate line: current Pass, Work Units merged / total, percentage of Pixel Samples (merged plus in flight), cluster Pixel Samples per second, elapsed time and ETA. Below it, one line per Worker: host, threads, Work Units in flight, its Pixel Samples per second, heartbeat age and state (`loading` with its progress, `alive`, `stale`, `stalled`, `dead`), and its Worker Slot's restart count. With no Worker alive, the aggregate line reads `waiting: no Workers`. Without a TTY, the same content is an `info` event every ~10 s, one per Worker. |
| No endpoint | No HTTP, Prometheus or other metrics endpoint. One may be added later behind the telemetry seam (`dyn`, §8.5) without touching the rest. |
| Heartbeat progress | Every heartbeat carries, per Work Unit in flight, the cumulative count of Pixel Samples completed, and the Worker's busy threads / total threads. Cumulative counts tolerate lost heartbeats. The Coordinator derives every rate; Workers never send rates. |
| ETA | During Pass 0: remaining Pixel Samples over an exponential moving average (~10 s) of the cluster rate, shown as provisional (`~`). From Pass 1: the sum of the remaining Work Units' Tile costs over the recent cluster throughput. A Tile's cost is its render time, measured by the Worker, sent in the Work Result and normalised by that Worker's rate. The duration is metadata outside the Work Result content hash (ADR 0004). |
| Stalled Worker | A Worker that sends heartbeats but whose Pixel Sample count, or loading progress while `loading`, has not grown for 30 s (Coordinator option) is shown as `stalled` and logged as a `warn`. Nothing is reassigned: scheduling stays as in ADR 0009, and the speculative tail covers it at the end. |
| Ray statistics | pbrt-v4-style counters (rays by kind, intersection tests, BVH nodes visited, path lengths) only with the Cargo feature `stats`, off by default and compiled away when off. Plain integers in the per-thread context (§8.7): no atomics, no allocation, never shared. Summed per Work Unit, sent in the Work Result, aggregated in the Render Job Report. Never on the status line. They do not touch the Film. |
| Render Job Report | One JSON file per Render Job, written next to the final image (`<image>.report.json`, path is a Render Job option). Umbra's own schema, versioned `major.minor`, with a JSON Schema generated from the Rust types (`serde` + `schemars`) and committed. It carries a copy of the Reproduction Record (§8.15) and the render time, which the image leaves out. |
| Report content | Render Job: options, `outcome` (`finished` or `cancelled`), Film hash (absent when cancelled), phase times (parse, Scene build, BVH build, Scene replication, render, merge, image write), Pixel Samples per second, Anomalous Pixel Sample count (§8.9). Per Worker: host, CPU, threads, version, Work Units accepted, Pixel Samples, busy and idle time, mean thread occupancy, Scene replication and BVH build time. Scheduling: reassignments, speculative copies (won, lost, cancelled), duplicate Work Results discarded, hash mismatches, scaling efficiency when standalone rates are known. Per Tile: normalised cost. Scene and BVH: primitives per type, BVH nodes, maximum and mean depth, primitives per leaf (maximum, mean), total SAH cost, memory per arena and of the BVH, always present. `stats` counters when the feature is on. |
| Cancelled Render Job | Ctrl-C still writes the Render Job Report, with `outcome: "cancelled"`, the phase times so far and the Pixel Samples merged, and no image (ADR 0004). `umbra bench` and `umbra validate` reject any report whose `outcome` is not `finished`. |
| Comparable fields | Only render-phase Pixel Samples per second and phase times are compared by `umbra bench` under the 2 % / 95 % CI rule (§8.4), plus scaling efficiency once the cluster is distributed. Everything else is informative. Changing the meaning of a comparable field bumps the schema major version. |
| Logs | `tracing` to stderr in the Coordinator and the Workers. Human-readable on a TTY, JSON lines otherwise; `--log-format=human\|json` forces one. `info` by default, with an `RUST_LOG`-style filter in `--log`. Workers have no status line: at `info` they log lifecycle events (joined, Scene received, BVH built, disconnected) and a summary of their Work Units in flight every ~10 s. |

Where each metric comes from and where it ends up:

```mermaid
flowchart LR
    W["Worker"] -->|"heartbeat, every 1 s:<br/>cumulative Pixel Samples,<br/>busy threads"| C["Coordinator"]
    W -->|"Work Result:<br/>render time, stats counters"| C
    C --> S["status line (stderr)<br/>aggregate + per Worker"]
    C --> L["tracing events<br/>(human or JSON lines)"]
    C --> R["Render Job Report<br/>(&lt;image&gt;.report.json)"]
    R --> B["umbra bench<br/>(comparable fields only)"]
```

## 8.11 Correctness validation

How Umbra proves it renders what pbrt-v4 renders ([ADR 0013](../adr/0013-statistical-validation-against-a-published-pbrt-v4-reference.md), [Correctness validation strategy](https://github.com/sergioffpc/umbra/issues/29)). Each layer catches what the one above cannot locate.

| Concern | Rule |
| --- | --- |
| Pinned reference | `mmp/pbrt-v4` at `b0bdf461ba5dd6504b70ebf0932acc0fe7aa744f`, `mmp/pbrt-v4-scenes` at `30cf4a0346ae5a80a2d7a530a3ef7d0fa4f70572`. One file holds both commits once validation code exists. Moving them is an explicit decision that publishes a new reference. |
| Reference statistics | A repository script builds the pinned pbrt-v4 and renders every Reference Scene 16 times (seeds 0–15) at its validation settings. Per-pixel mean and variance EXRs, and the Reference Image of QS-1 (§10), are published as assets of the GitHub release `pbrt-v4-reference-<commit>`, downloaded and hash-checked by `umbra validate`. |
| Image test | Umbra renders the same 16 seeds. Per pixel and RGB channel, a Welch t-test. Benjamini–Hochberg at q = 0.01 over all tests: one rejection fails the Reference Scene. Zero variance on both sides passes only with equal means. Any Anomalous Pixel Sample fails it too. |
| Report | Per Reference Scene: verdict, p-value map, rejected pixels, Anomalous Pixel Samples as `umbra trace` commands (§8.9), and relMSE and FLIP as diagnostics. |
| Noise target | The Welch test detects bias. Whether Umbra is noisier than pbrt-v4 is QS-1 (§10): relMSE against the Reference Image, checked by the milestone's benchmark measurement issue. Both block the milestone. |
| Where | By hand on a target machine with `umbra validate`, never in CI. |
| Golden Film hashes | CI renders each Reference Scene of the current milestone at minimal settings (e.g. 32 × 32, 4 Pixel Samples per pixel) on arm64 and x86-64 and compares Film hashes with committed values. Scenes are downloaded from the pinned commit and cached in CI. No images are committed. |
| When to validate | A pull request that changes a golden Film hash updates the committed hashes and attaches a passing `umbra validate` report for the affected Reference Scenes. Every milestone closes with a manual validation issue next to the benchmark issue; its report goes to `docs/validation/<milestone>.json`. |
| Sampling tests | In `cargo test`, every variant of a sampling interface (BxDF, Light, Shape, PhaseFunction, sampling distributions) passes four tests with fixed seeds: a χ² goodness-of-fit of sampled directions against the PDF integrated over a (θ, φ) grid, low-count cells merged and the p-value corrected for the number of tests; `sample` and `pdf` agree; the PDF integrates to 1; for BxDFs, reciprocity where physics requires it and albedo ≤ 1. A new variant joins them, as it joins the allocation (§8.7) and tracing (§8.9) tests. |
| Optimisation equivalence | An aggressive optimisation that keeps operations and their order (SIMD, hoisted dispatch, `unsafe`) is bit-identical to its reference: differential `proptest` tests with generated edge cases (NaN, ±∞, signed zeros, denormals, axis-parallel rays) and one Reference Scene with the same Film hash on both paths. One that changes results by nature (polynomial approximations, ULP-space offsets, a non-deterministic BVH builder) declares an error bound in its contract (e.g. ≤ 2 ulp against `libm`), checked by `proptest` or exhaustively over the `f32` domain where feasible, and passes `umbra validate`. |
| References kept | The readable reference stays as an ordinary function and is never deleted. The Cargo feature `reference` renders with references end to end, for debugging. |
| Unit tests | When a module lands, its pbrt-v4 `*_test.cpp` cases are ported to idiomatic Rust, each naming its source file and test. Tests of C++-only machinery (`taggedptr`, `buffercache`, `pstd`) are not ported. Module-specific tests come on top. |

Which layer answers which question:

```mermaid
flowchart TD
    Q1["Is a routine's sampling consistent<br/>with its PDF?"] --> S["sampling tests (cargo test, CI)"]
    Q2["Does an optimisation still compute<br/>what its reference computes?"] --> E["equivalence tests (cargo test, CI)"]
    Q3["Did this pull request change pixels?"] --> H["golden Film hashes (CI)"]
    H -->|"hash changed"| V["umbra validate (by hand)"]
    Q4["Is the image unbiased<br/>with respect to pbrt-v4?"] --> V
```

## 8.12 GPU readiness

What the CPU code commits to so that a GPU port does not mean a rewrite ([ADR 0014](../adr/0014-gpu-ready-no-std-rendering-core.md), [GPU-readiness constraints](https://github.com/sergioffpc/umbra/issues/32)). `f32` only (§8.1), closed enum dispatch (§8.5) and allocation-free index handles (§8.7) are part of it and are not repeated here.

| Concern | Rule |
| --- | --- |
| Rendering core | One `#![no_std]` crate without `alloc` holds all per-Pixel-Sample code: geometry, spectra, Shapes, BxDFs, Materials, textures, Lights, media, Samplers, Camera, Filter, BVH traversal and the per-sample body of each integrator. CI builds it for `aarch64-unknown-none`. Its dependencies are `no_std` (`libm`, `wide` without default features). |
| Scene access | The core defines `SceneView<'a>`, a `Copy` struct of borrowed arena slices. The owning `Scene`, `SceneBuilder`, BVH construction and the per-thread contexts live outside the core and lend slices to it. |
| Recursion | None on the per-sample path. Fixed-size explicit stacks (BVH traversal `[u32; 64]`) and iterative integrators. Checked in review. |
| Panics | Never control flow, never caught. Bounds checks and `debug_assert!` stay; a panic is a bug. |
| Values | Per-sample values and arena elements are `Copy`. `#[repr(C)]` + `bytemuck::Pod` only where a byte layout is already needed (Tile pixels on the wire). |
| Determinism | Bit-identical results are a guarantee across CPU Workers only (§8.3). A GPU backend decides its own. |
| Deferred | Wavefront driver, SoA derive, GPU toolchain (rust-gpu, Rust-CUDA or hand-written shaders), `TextureEvaluator`, tag-plus-handle split of the `*Kind` enums for GPU sharing. |

Where the core boundary falls:

```mermaid
flowchart LR
    subgraph host["outside the core (std)"]
        S["Scene, SceneBuilder,<br/>BVH build"]
        T["per-thread context<br/>(Vec buffers)"]
        I["dyn Integrator,<br/>transport, writers"]
        R["TraceRecorder"]
    end
    subgraph core["rendering core (no_std, no alloc)"]
        V["SceneView&lt;'a&gt;"]
        K["Shapes, BxDFs, Lights, textures,<br/>media, Samplers, Camera, Filter"]
        B["BVH traversal"]
        P["per-sample integrator body<br/>generic over R: Recorder"]
    end
    S -->|"view()"| V
    T -->|"&mut slices"| P
    I -->|"calls per Pixel Sample"| P
    R -.->|"implements Recorder"| P
    P --> K
    P --> B
    B --> V
```

## 8.13 Wire protocol

The messages between the Coordinator and its Workers and how they travel over QUIC ([ADR 0010](../adr/0010-quic-transport-between-coordinator-and-workers.md), [ADR 0015](../adr/0015-one-render-job-per-quic-connection.md), [Wire protocol between Coordinator and Workers](https://github.com/sergioffpc/umbra/issues/47)).

| Concern | Rule |
| --- | --- |
| Connection | One Render Job per connection. The Worker is the client and connects to the Coordinator's UDP port, 7470 by default. Workers receive `--coordinator <host:port> --session <id>` from `scripts/workers up`, which starts them only once the Coordinator has parsed the Scene and listens (ADR 0018). No retry and no reconnection: a Worker whose first connection fails, or that loses the Coordinator, exits, and one that comes back (restarted by the script) joins as a new Worker (§8.8). |
| TLS | The Coordinator generates a self-signed certificate at start-up; Workers accept any certificate. TLS exists only because QUIC requires it. |
| Encoding | Control messages, Scene structure and Work Result headers are `postcard` with a length prefix. Bulk data (assets, pixel sums, splats) is raw little-endian bytes through `bytemuck`. No protocol version: both sides are on the same commit (ADR 0012). |
| Handshake | `Hello { session_id, build_identity: { commit, dirty, target_cpu }, host, threads }`. The Coordinator answers `Reject { reason }` (`SessionMismatch`, `CommitMismatch { coordinator_commit }`, `DirtyBuild` without `--allow-dirty`) or `Welcome { worker_id }`. A rejected Worker logs the reason and exits non-zero. |
| Scene transfer | After `Welcome`, the Coordinator streams to each Worker a header with the Render Job options the Worker needs (seed, Tile size, Pixel Samples per Pass, total Pixel Samples per pixel), the Scene structure, and then every asset with its content hash, which the Worker checks. No asset cache. |
| Readiness | The Worker's first `RequestWork`, sent once the Scene is loaded and its BVH built, is the readiness signal. Until then the status line (§8.10) shows the Worker as `loading`. |
| Credits | `RequestWork { count }` adds `count` to the Worker's pending demand. The Coordinator sends `Assign { work_unit: { tile_index, pass } }` whenever it has a Work Unit for that demand and never answers "nothing". The Worker sends one credit back for each finished Work Result and each `Cancel`. Pixel ranges derive from the Render Job options and never travel. |
| Cancel | `Cancel { work_unit }`: the Worker drops the Work Unit if it is still rendering it (checked between Tile rows) and sends nothing. If the Work Result is already being sent, the Coordinator stops its stream with `stop_sending`. |
| Work Result | Header: the Work Unit, a 64-bit content hash, the Anomalous Pixel Samples (pixel, sample index, kind), a Film kind; outside the hash, render time and the `stats` counters. Body, for `RGBFilm`: per pixel of the Tile in raster order `rgb_sum: [f64; 3]` and `weight_sum: f64` (32 KiB for a 32 × 32 Tile), then an optional image-sized `rgb_splat: [f64; 3]` accumulator, absent until BDPT. The hash is `xxh3-64` over the body and the Anomalous Pixel Samples; the Worker computes it and the Coordinator checks it on receipt. |
| Heartbeat | From `Welcome` on, every second, from a task independent of loading and rendering (ADR 0018). While loading: Scene bytes received and the phase (`receiving`, `building BVH`). Afterwards: per Work Unit in flight, the cumulative Pixel Samples completed, and busy / total threads (§8.10). |
| Fatal error | A Worker that cannot continue (asset hash mismatch, out of memory, a panic caught at its top level) sends `Fatal { message }` and closes. The Coordinator logs it and treats the Worker as dead (§8.8). |
| End | `JobFinished` when the last Work Unit is merged: the Worker drops its remaining copies, closes and exits 0. The Coordinator writes the image and Render Job Report and exits once its Workers have closed, or after 2 s. Losing the Coordinator is detected by the QUIC idle timeout, set to the heartbeat death time (5 s). |
| Close codes | `FINISHED`, `JOB_CANCELLED` (Ctrl-C on the Coordinator: no image, ADR 0004), `REJECTED`, `WORKER_FATAL`, `WORKER_TIMED_OUT` (the Coordinator declared the Worker dead after 5 s without a heartbeat), `WORKER_STOPPED` (Ctrl-C or `SIGTERM` on the Worker), `PROTOCOL_ERROR` (any malformed or out-of-order message, always a bug). |
| Cancellation | Ctrl-C (`SIGINT`) on the Coordinator closes every connection with `JOB_CANCELLED`, which ends every stream, so no `Cancel` per Work Unit is sent. The Coordinator writes the Render Job Report without an image (§8.10) and exits 130 once its Workers have closed, or after 2 s. A second Ctrl-C exits at once. Workers drop every Work Unit in flight and exit 130. |
| Worker stop | Ctrl-C or `SIGTERM` on a Worker closes its connection at once with `WORKER_STOPPED`, dropping its Work Units in flight (no drain), and the Worker exits 130. The Coordinator treats it as dead (§8.8) and the Render Job continues. |
| Exit codes | Every `umbra` subcommand: `0` success; `1` failure that a restart may cure (`Fatal`, a panic caught at the top level, `WORKER_TIMED_OUT`, Coordinator lost or unreachable); `2` invalid usage (`clap`); `3` refused, where a restart cannot help (Diagnostics, `Reject`, `PROTOCOL_ERROR`); `130` cancelled or stopped by the operator (`JOB_CANCELLED`, `WORKER_STOPPED`, Ctrl-C). `scripts/workers up` restarts a Worker Slot on any exit status except `0`, `2`, `3` and `130` (ADR 0018), which also covers deaths the Worker cannot report itself: an uncaught panic (`101`), an OOM kill (`137`), a lost SSH session (`255`). |
| Local Worker | The in-memory transport passes the same typed messages without encoding them or recomputing hashes. Encoding is covered by round-trip tests and by an integration test with two QUIC Workers on `localhost`. |

Streams of one connection:

| Stream | Opened by | Direction | Carries |
| --- | --- | --- | --- |
| Control (bidirectional, one) | Worker | both | `Hello`, `Welcome`, `Reject`, `RequestWork`, `Assign`, `Cancel`, `Fatal`, `JobFinished`, in order |
| Scene (unidirectional, one) | Coordinator | C → W | Render Job options, Scene structure, assets; end of stream ends the Scene |
| Heartbeat (unidirectional, one) | Worker | W → C | one heartbeat per second |
| Work Result (unidirectional, one per Work Result) | Worker | W → C | one Work Result; end of stream ends it |

Heartbeats are not QUIC datagrams, because a burst of lost datagrams would declare a live Worker dead. Work Results each get their own stream, so a large one never delays another, a heartbeat or a `Cancel`.

```mermaid
sequenceDiagram
    participant W as Worker
    participant C as Coordinator
    W->>C: Hello (session, Build Identity, threads)
    C->>W: Welcome (worker_id)
    C-->>W: Scene stream: options, structure, assets
    Note over W: load Scene, build BVH
    W->>C: RequestWork (count)
    C->>W: Assign (Tile, Pass)
    W-->>C: heartbeat stream, every 1 s
    W-->>C: Work Result stream
    W->>C: RequestWork (1)
    C->>W: JobFinished
    W->>C: close (FINISHED)
```
## 8.14 Per-architecture tuning

How Umbra is tuned to each target machine (§7.1) beyond the portable defaults ([Per-architecture tuning plan](https://github.com/sergioffpc/umbra/issues/48), [ADR 0017](../adr/0017-one-worker-per-numa-node.md)). The plan is an evidence-driven backlog, not a schedule. Every entry is an aggressive optimisation: a benchmark proves it (§8.4), a simple contract isolates it, and a readable reference stays next to it (§8.11).

| Concern | Rule |
| --- | --- |
| Threads | A Worker uses every logical CPU of its NUMA node by default: 64 on the P620, 28 per Worker on the R630 and 12 on the M2 (P- and E-cores). `--threads N` overrides it and never changes the Film (§8.3). No thread pinning and no QoS class. |
| Workers per machine | One per NUMA node, bound with `numactl --cpunodebind/--membind`, so two on the R630 and one elsewhere ([ADR 0017](../adr/0017-one-worker-per-numa-node.md)). Before distribution, the R630's local Worker runs under `numactl --interleave=all`. |
| Fixed host configuration | The P620's BIOS stays in NPS1 (one NUMA node). SMT stays on on both x86 machines. Changing either invalidates comparisons with earlier benchmark reports. |
| One BVH everywhere | Every architecture builds and traverses the same BVH layout, so traversal-level Pixel Traces reproduce on any machine (§8.3, §8.9). A wider BVH is a backlog entry for all architectures at once, never for one. |
| Triggers | An entry is reopened when a machine misses its QS-5 (speed relative to pbrt-v4) or QS-6 (scaling within a Worker) target (§10). A QS-6 miss reopens that machine's threading and memory entries. A QS-5 miss reopens the kernel entries of its architecture. |
| Selection | A trigger says that work is needed, not which entry. The entry is chosen from a profile that points at its cause (samply, Instruments, `perf`, §8.4), never by going down the list. An entry may also be taken without a trigger when a profile justifies it, because QS-5 must reach 1.0 by the last milestone. |
| Acceptance | The end-to-end gain rule of §8.4, measured on every machine the entry targets. An entry that targets all architectures must also not regress any other machine by more than 2 %. |

The backlog, in the order its entries are worth trying:

| # | Entry | Machines | Trigger and evidence | Cost |
| --- | --- | --- | --- | --- |
| 1 | BVH8 traversal with `f32x8` (two NEON registers on arm64) | all | QS-5 miss on x86, profile dominated by traversal | code, equivalence tests (§8.11) |
| 2 | Scene and BVH replica per NUMA node inside one Worker, one rayon pool per node | R630 | QS-6 miss with two Workers | code, amends ADR 0008 and ADR 0017 |
| 3 | Pinning pool threads to cores | P620, R630 | QS-6 miss, profile showing thread migrations | `unsafe` or a crate (§8.6) |
| 4 | One rayon pool per CCX, each Work Unit in flight confined to one CCX | P620 | QS-6 miss, profile showing L3 misses across CCXs | code |
| 5 | Huge pages for the Scene arenas and the BVH (`madvise`) | P620, R630 | profile showing TLB misses | `unsafe` or a crate (§8.6) |
| 6 | Excluding SMT siblings or E-cores (`--threads`) | all | benchmark | none |
| 7 | `USER_INITIATED` QoS class for pool threads | M2 | QS-5 or QS-6 miss on the M2 | FFI, `unsafe` (§8.6) |

Rejected: an AVX-512 path (no target machine has it, §7.1), one Worker per CCX on the P620 ([ADR 0017](../adr/0017-one-worker-per-numa-node.md)), and a different BVH width per architecture. Ray-packet and wavefront kernels are deferred with the wavefront driver (§8.12).

## 8.15 Image output

What a Render Job writes as its final image ([ADR 0020](../adr/0020-bit-identical-exr-final-image.md), [Image output](https://github.com/sergioffpc/umbra/issues/65)).

| Concern | Rule |
| --- | --- |
| Film | `Film "rgb"` with every pbrt-v4 parameter: `xresolution`, `yresolution`, `cropwindow`, `pixelbounds`, `diagonal`, `filename`, `savefp16`, `sensor` (named camera sensors included), `iso`, `whitebalance`, `maxcomponentvalue`. `"gbuffer"` and `"spectral"` are rejected with a Diagnostic until their family's milestone. |
| Format | OpenEXR only. The extension of `filename`, or of the `--outfile` override, must be `.exr`. Anything else is rejected by Scene validation before any Work Unit (§8.2). |
| Path | `--outfile`, else the Film's `filename`, else `pbrt.exr`, resolved as in pbrt-v4. The Render Job Report is written next to it (§8.10). |
| Header | pbrt-v4's `RGBFilm` metadata: `worldToCamera`, `worldToNDC`, `pixelBounds` (data window), `fullResolution` (display window), `samplesPerPixel`, `chromaticities`. No `renderTimeSeconds` (it is in the Render Job Report) and no `MSE`. Plus `umbra.reproductionRecord`: one text attribute holding the Reproduction Record as versioned JSON. |
| Pixels | Half or float as `savefp16` says, RGB channels only. |
| Determinism | ZIP compression, `LineOrder::Increasing`, fixed attribute order: the file bytes are a pure function of the Render Job (§8.3). A test hashes the file, not only the Film. |
| Encoder | The `exr` crate (pure Rust, no `unsafe`, §8.6), behind the `dyn` image-writer seam of `umbra-render` (§8.5). The same crate reads images for `umbra trace` and `umbra validate`. |
| Writing | On the Coordinator, once every Pass is merged: temporary file in the target directory, `fsync`, rename. An existing file is overwritten, as in pbrt-v4. A cancelled or failed Render Job writes no image, only its Render Job Report. |
