# A layered workspace with a seven-crate `no_std` core

Umbra is a Cargo workspace of twelve crates. Seven of them form the `no_std` core, and five run on `std`. Decided in [Code organisation and module boundaries](https://github.com/sergioffpc/umbra/issues/30). A crate boundary exists only where something concrete differs on each side: the environment (`no_std` or `std`), a heavy dependency (`nom`, `tokio`, `quinn`), or a contract narrow enough to test on its own. Each crate's interface and dependencies are in [arc42 §5](../architecture/05-building-block-view.md).

- **The core is seven crates, layered in pbrt-v4's dependency order.** They are `umbra-math`, `umbra-sampling`, `umbra-spectrum`, `umbra-geometry`, `umbra-shading`, `umbra-camera` and `umbra-integrators`. Every one is `#![no_std]` without `alloc`, so the rules of [ADR 0014](0014-gpu-ready-no-std-rendering-core.md) apply to "the core crates" rather than to one crate. CI builds all seven for `aarch64-unknown-none`.
- **IDs break pbrt-v4's cycles.** In pbrt-v4, `SurfaceInteraction` points at its Material, area Light and Medium, Materials need Textures and BxDFs, and Lights need Shapes. In Umbra, interactions in `umbra-geometry` carry `u32` IDs (`MaterialId`, `LightId`, `MediumId`, [ADR 0008](0008-index-handle-arenas-and-allocation-free-rendering.md)). The function that turns an interaction into a BSDF lives in `umbra-shading`. Camera and shading are parallel branches: the Camera never needs a Material.
- **Book chapters are modules, not crates.** Inside a core crate, each pbrt-v4 chapter or section is a module (`umbra_shading::bxdfs`, `umbra_shading::lights`). Splitting further would create interfaces that are always used together.
- **Outside the core**: `umbra-scene` (owning Scene, `SceneBuilder`, BVH construction, Render Job options), `umbra-parser` (the `.pbrt` adapter), `umbra-render` (one process renders Work Units, owns the Film's merge order and Pixel Traces), `umbra-cluster` (Coordinator and Worker behind a synchronous interface that hides `tokio`) and `umbra` (the `clap` binary).
- **The binary holds `bench` and `validate`.** Their statistics (Welch, Benjamini–Hochberg, confidence intervals) and report comparisons are modules of `umbra`, not a crate of their own. They run Render Jobs through `umbra-cluster` with a local Worker, so what they measure is the real path.
- **Scene replication goes through `serde`.** Every core crate has a `serde` feature, off by default, that derives `Serialize`/`Deserialize` with `default-features = false`. The Coordinator sends the builder's arenas without the BVH, and each Worker builds its own BVH ([ADR 0003](0003-pbrt-v4-scene-files-parsed-on-the-coordinator.md)).
- **One workspace configuration.** Version, edition, lints and external dependency versions are declared once at the root and inherited, and every crate is `publish = false`. The Build Identity is the commit, not the version ([ADR 0012](0012-per-cpu-binaries-locked-to-one-commit.md)).

## Considered options

- **Two crates**, a `no_std` core and one `std` crate holding everything else: rejected. Rendering would compile next to `tokio`, `quinn` and `nom`, and `pub(crate)` would no longer separate the parser, the renderer and the cluster.
- **One core crate**, with the chapters as modules: rejected. Geometry and BVH traversal, the first optimisation targets, could not be tested and benchmarked without Materials and Lights, and nothing would stop a Shape from reaching into shading.
- **One crate per chapter** (about 24 crates, as pbrt-v4's directories suggest): rejected. pbrt-v4's interaction, Material, Light and Shape cycles would force invented "common" crates, `SceneView` and the `*Kind` enums need every concrete type anyway, and everything crossing a crate would have to be `pub`.
- **A separate crate for `bench` and `validate`**: rejected in favour of keeping them in the binary, which is the only caller.
- **Mirror DTOs in `umbra-cluster` for replication**: rejected. Every new Shape, Material or Light would be written twice, and a forgotten field would fail only at runtime.

## Consequences

- Moving a type between core crates is a breaking change to their interfaces. The layer order (math, sampling, spectrum, geometry, shading and camera, integrators) is the hard part to reverse.
- Every core dependency must build without `std`, and with the `serde` feature on as well as off.
- Cross-crate tests (golden Film hashes, the trace oracle, the counting allocator) live in `crates/umbra/tests/`, the only crate that sees everything.
