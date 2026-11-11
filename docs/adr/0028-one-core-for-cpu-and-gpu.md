# One core for CPU and GPU

The `no_std` core is compiled twice from the same source: for `x86_64` and for `nvptx64-nvidia-cuda`, both by upstream `rustc`. Two drivers run it, a per-pixel loop on the CPU and a wavefront on the GPU. Decided with the owner on 2026-11-11. This amends [ADR 0014](0014-gpu-ready-no-std-rendering-core.md), [ADR 0015](0015-layered-workspace-with-a-seven-crate-no-std-core.md), [ADR 0022](0022-determinism-on-one-machine-statistical-equality-across-machines.md) and [ADR 0023](0023-x86-64-only.md), and makes concrete the GPU port that ADR 0014 deferred.

- **Upstream `rustc` and `nvptx64`.** `rustc` emits PTX through LLVM's NVPTX backend, and the host loads it with `cudarc` and OptiX. No third-party codegen backend is involved. rust-gpu targets SPIR-V, which an NVIDIA-only GPU does not need. Rust-CUDA's NVVM backend is reconsidered only if a benchmark shows upstream LLVM falling clearly short, and the switch would touch `umbra-kernels` only. Data-carrying enums compile on this path, so the tag-plus-handle split of the `*Kind` enums that ADR 0014 deferred is not needed.
- **The core works on values.** Each step of a path (camera ray, intersection, BSDF and light sampling, emission) is a core function that takes and returns `Copy` values. No core function takes a queue, a buffer or a thread index. The CPU driver calls these steps in a loop per pixel; the GPU driver calls them from one kernel per stage and owns the structure-of-arrays queues between stages. Review checks this rule, as it checks recursion.
- **A fixed layout across the host-device boundary.** Every type stored in a `SceneView` arena is `#[repr(C)]`, and every Scene-resident `*Kind` enum is `#[repr(C, u32)]`, whose layout is defined for data-carrying enums. The host copies arena slices into GPU buffers byte for byte. The existing `size_of` assertions, plus `offset_of` assertions on fields where layout matters, compile for both targets. Per-sample values that live in registers keep `repr(Rust)`. This reverses ADR 0014's rejection of `#[repr(C)]` for arena types; `bytemuck::Pod` is still not required.
- **`f64` only inside `umbra-math`'s backends.** The scalar transcendentals that evaluate in `f64` move into the `x86_64` backend, where an `f64` FMA costs the same as an `f32` one. Outside `umbra-math`, `clippy::disallowed_types` forbids `f64` in the core. On consumer GPUs `f64` runs at a small fraction of `f32`'s rate.
- **An `nvptx64` backend for `umbra-math`.** It implements the `Vec4`/`Mask4` facade as plain scalar lanes, since the GPU vectorises across threads, and the scalar primitives with PTX instructions. Its transcendentals may use PTX's approximate instructions (`sin.approx`, `ex2.approx` and the like). Each one's error bound is documented and checked by the GPU test script ([ADR 0026](0026-gpu-workers-from-the-first-milestone.md)). ADR 0022's ban on approximate instructions keeps CPU Films the same across CPU vendors, and does not apply to the GPU, whose results are statistical only.
- **Two new crates.** `umbra-kernels` is `#![no_std]`, entirely under `#![cfg(target_arch = "nvptx64")]`, so a host build of the workspace compiles it empty. It holds the wavefront kernels, the OptiX entry points and the OptiX device bindings in inline PTX. `umbra-gpu` is a `std` crate: `cudarc`, the OptiX host side (acceleration structures, pipeline, shader binding table), the queues, the `SceneView` upload and the GPU Worker's renderer. Its build script compiles `umbra-kernels` for `nvptx64` into a target directory of its own and embeds the PTX.
- **Panics trap.** `umbra-kernels` sets a `#[panic_handler]` that executes `trap`. A panic is a bug, as ADR 0014 already says.
- **`no_std` checked again.** CI builds `umbra-kernels` for `nvptx64`, which has no `std`. That brings back the automated check that ADR 0023 dropped with the `aarch64-unknown-none` build: a core dependency that pulls `std` breaks the build. `alloc` may still be available on that target, so keeping it out stays a matter of review and of the counting-allocator test of ADR 0008.

## Considered options

- **Rust-CUDA**: rejected. It depends on an external codegen backend that sat unmaintained for years, pinned to its own nightly.
- **Hand-written CUDA C++**: rejected. It shares no code with the core, so it would be a second implementation.
- **One wavefront driver for both**, as pbrt-v4 can run it on the CPU: rejected. It is slower on the CPU than the per-pixel loop, which Pixel Traces also rely on.
- **A core generic over its storage** (a `Storage` trait with AoS and SoA implementations): rejected. It spreads type parameters through the core for what the values rule gives without them.
- **Mirror types for the GPU**, converted by the host: rejected. Two sets of types to keep in sync, and the GPU would no longer run the same core.
- **Relying on `repr(Rust)`** giving the same layout on two 64-bit targets: rejected. It is not guaranteed and would fail silently.
- **`f32` everywhere, CPU included**: rejected. Pure-`f32` transcendentals within 1 ULP need more operations and careful range reduction, for no gain on the CPU.
- **Pure-`f32` GPU transcendentals without approximate instructions**, testable on the CPU in CI: rejected by the owner in favour of speed.
- **One `umbra-gpu` crate with the device code in a module**, or the kernels inside `umbra-integrators`: rejected. The first mixes `std` and `no_std` in one crate; the second mixes OptiX `asm!` into the shared core.

## Consequences

- `repr(C)` does not reorder fields, so arena types may grow padding. The `size_of` assertions catch it, and fields are ordered by hand.
- Code that orchestrates a path exists twice, once per driver. The steps it calls exist once.
- The workspace grows from twelve to fourteen crates; the core keeps its seven.
