# One crate per Render Device over a shared contract

> **Amended on 2026-11-13 by [#127](https://github.com/sergioffpc/umbra/issues/127).** The two `Renderer`s no longer have the same shape. `umbra_gpu::Renderer::new` also takes the GPU's ordinal, and `render` takes a batch of Work Units, the `Renderer::CAPACITY` Work Units a GPU Worker keeps in flight ([ADR 0026](0026-gpu-workers-from-the-first-milestone.md)), and returns one `Rendered` per Work Unit, or an `Error` when CUDA or OptiX fails. The GPU's `Renderer` copies what it reads of the Scene to the device and borrows nothing, so it has no lifetime. Until the GPU Local Worker ([#128](https://github.com/sergioffpc/umbra/issues/128)), `umbra-cluster`'s enum holds the CPU's `Renderer` alone and does not depend on `umbra-gpu`. The rest of this ADR still applies.
>
> **Amended on 2026-11-14 by [#128](https://github.com/sergioffpc/umbra/issues/128).** `umbra-cluster` depends on `umbra-gpu` and holds no enum over the two `Renderer`s: a Worker runs the render loop of its Render Device, the CPU's with Work Units rendered several at once, the GPU's with them rendered as one batch. The rest of this ADR still applies.

The code that renders Work Units is split three ways: what is the same on every Render Device, what only the CPU does, and what only the GPU does. `umbra-render` keeps the first, `umbra-cpu` and `umbra-gpu` hold the other two, and both depend on `umbra-render` alone of the three. `umbra-kernels` becomes `umbra-gpu-programs`, a normal dependency of `umbra-gpu` that also holds the host-device layout. Decided with the owner on 2026-11-13 in [#184](https://github.com/sergioffpc/umbra/issues/184). This amends [ADR 0015](0015-layered-workspace-with-a-seven-crate-no-std-core.md), [ADR 0027](0027-a-pinned-nightly-toolchain.md) and [ADR 0028](0028-one-core-for-cpu-and-gpu.md).

Before this ADR, `umbra-render` mixed the device-neutral contract (Work Units, Work Results, the Film and its merge, the final image, the Reproduction Record) with the CPU's renderer and its `rayon`. The GPU's renderer depended on `umbra-render` for the contract and arrived through the CPU's crate. The host and the device shared their layout through `#[path]` on two source files of `umbra-kernels`.

The decision:

- **`umbra-render`: the contract.** `Decomposition`, `WorkUnit`, `WorkResult`, `Film` and its merge order, `Image`, `ReproductionRecord`, `Cancellation`, `Rendered`. It depends on neither `rayon` nor anything from NVIDIA.
- **`umbra-cpu`: the CPU's renderer.** Today's `Renderer`: `rayon` over a Work Unit's rows, calling `umbra_integrators::evaluate_pixel_sample` per Pixel Sample. Pixel Traces re-execute through it ([ADR 0011](0011-pixel-traces-by-deterministic-re-execution.md)).
- **`umbra-gpu`: the GPU's renderer, host side.** `cudarc`, OptiX's host side, the `SceneView` upload, the wavefront queues. Its build script compiles `umbra-gpu-programs` for `nvptx64` and embeds the PTX.
- **`umbra-gpu-programs`: the GPU's renderer, device side.** It compiles for the host and for `nvptx64`. On both it holds the layout the two sides share (`LaunchParams`, the ray and hit records, the math program's parameters), so `umbra-gpu` uses those types through an ordinary dependency, with no `#[path]`. On `nvptx64` only it holds the OptiX programs, the wavefront stages, the OptiX device bindings in inline PTX and the trapping panic handler, and enables its unstable features through `cfg_attr`. Its library is an `rlib`; the build script asks for a `cdylib` with `cargo rustc --crate-type cdylib`.
- **The same shape on both Render Devices.** `umbra_cpu::Renderer` and `umbra_gpu::Renderer` take a `&Scene` and `&RenderJobOptions` and turn a `WorkUnit` into `Rendered`. `umbra-cluster` chooses between them with an enum over the two, by the Worker's Render Device.
- **The reference composition stays in the core.** `umbra-integrators` keeps the steps of each integrator and `evaluate_pixel_sample`, the composition of those steps for one Pixel Sample. `umbra-cpu` calls it as it is. `umbra-gpu-programs` composes the same steps as a wavefront, checked statistically against it ([ADR 0026](0026-gpu-workers-from-the-first-milestone.md)).
- **Architectures are not Render Devices.** The x86-64, arm64 and `nvptx64` backends of `umbra-math` stay private modules behind its facade. Which architecture is chosen at compile time, inside one function, and the backends share kernels and primitives across architectures in groupings that differ per function, so no crate boundary fits them. A Render Device is chosen at run time by the Worker, and each pulls in its own dependencies and build.

## Considered options

- **`umbra-render` absorbing both renderers as modules**: rejected. A crate boundary is what makes both renderers use only the contract's public API. The device half would also put the whole crate under `cfg` for `nvptx64`, and every build of the Film would build PTX.
- **`umbra-gpu` absorbing the device code**: rejected. Its build script would compile its own crate for `nvptx64`, a build that recurses into itself.
- **A `umbra-arch` crate holding every architecture's code**, used by `umbra-math` and the GPU's programs: rejected, for the reasons in the last point of the decision. The OptiX device API is OptiX's, not `nvptx64`'s, and would bring unstable features under `umbra-math`.
- **A shared composition per device only** (`umbra-integrators` holding the steps alone, each Render Device composing them): rejected. The integrator would exist nowhere as one piece, and Pixel Traces re-execute that piece.
- **Keeping the name `umbra-kernels`**: rejected. `umbra-math` calls its numerical routines kernels; the crate holds OptiX programs. `umbra-gpu-device` was rejected because Render Device names the CPU too, `umbra-gpu-shaders` because shading means something else in the core, `umbra-optix` and `umbra-wavefront` because each names one part of what the crate holds.

## Consequences

- The workspace has fifteen crates; the core keeps its seven.
- `umbra-gpu-programs` is the only crate with unstable features, on `nvptx64` only. A host build compiles it without them.
- CI's `no_std` check builds `umbra-gpu-programs` for `nvptx64`, as it built `umbra-kernels`.
- A change to the Film rebuilds no PTX.
