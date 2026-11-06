# A GPU-ready `no_std` rendering core

Umbra has no GPU backend, but its CPU code is written so that a later port does not mean a rewrite. Several of the rules that make this possible are already decided: `f32` only ([ADR 0001](0001-numeric-model.md)), closed enum dispatch with no `dyn` on the per-ray path ([ADR 0006](0006-closed-enum-dispatch-for-rendering-interfaces.md)), and `u32` index handles with no per-sample allocation ([ADR 0008](0008-index-handle-arenas-and-allocation-free-rendering.md)). This ADR adds the rest. The rules are chosen for the most restrictive Rust GPU toolchain (rust-gpu: no allocation, no recursion, no dynamic dispatch, no unwinding), so that every toolchain stays open ([pbrt-v4 CPU/GPU shared-code architecture](https://github.com/sergioffpc/umbra/issues/15)).

- **A `no_std` core without `alloc`.** All code on the per-Pixel-Sample path lives in one crate marked `#![no_std]` that does not link `alloc`. That covers geometry, spectra, Shapes, BxDFs, Materials, textures, Lights, media, Samplers, the Camera, the Filter, BVH traversal and the per-sample body of each integrator. CI builds the crate for `aarch64-unknown-none`, a target with no `std`, so a dependency on `std` or `alloc` breaks the build. The core's dependencies must be `no_std` too (`libm`, and `wide` with `default-features = false`). The `Recorder` trait is defined in the core. `TraceRecorder`, which allocates, lives outside it (ADR 0011). Which crates exist and what each is named is decided with the code organisation.
- **The core sees a `SceneView`.** An owning `Scene` holds `Vec` arenas, so it cannot live in a crate without `alloc`. The core defines `SceneView<'a>`, a `Copy` struct of borrowed slices (`&'a [Sphere]`, `&'a [Point3f]`, the BVH nodes, …), and per-sample code receives that instead of `&Scene`. The owning `Scene`, the `SceneBuilder` and BVH construction live outside the core, and `scene.view()` lends the slices. On a GPU those slices become buffers.
- **No recursion.** Per-sample code never calls itself directly or indirectly. Stacks are explicit arrays of fixed size, such as the BVH traversal stack `[u32; 64]`, and integrators loop over path vertices as pbrt-v4 does. No lint detects recursion, so code review checks it.
- **Panics mean bugs.** The core never uses a panic for control flow and never relies on unwinding or `catch_unwind`. Bounds-checked indexing and `debug_assert!` stay, and a panic they raise is a bug to fix. The core is not required to be panic-free.
- **`Copy` values.** Per-sample values and the elements of every Scene arena are `Copy`: no `Drop`, no references, no owned heap data. `#[repr(C)]` and `bytemuck::Pod` are not required, since `Pod` forbids padding and data-carrying enums. They are used only where something already needs a fixed byte layout, such as Tile pixels on the wire.
- **Determinism is a CPU guarantee.** Bit-identical Work Results and Films ([ADR 0002](0002-bit-identical-results-across-architectures.md), [ADR 0004](0004-deterministic-film-for-a-fixed-render-job.md)) are promised across CPU Workers only. A GPU has its own transcendentals, FMA contraction and reduction order. Whether a GPU backend joins that guarantee, or produces statistically equal results instead, is decided when one is built. The CPU code is not constrained for it now.

## Deferred

These belong to a GPU port and are not built or designed now:

- **The wavefront driver**: pbrt-v4's `WavefrontPathIntegrator` with its kernels per stage and `WorkQueue`s. The CPU integrators stay `dyn Integrator` (ADR 0006).
- **SoA layout**: a derive that generates structure-of-arrays types, like pbrt-v4's `soac`. Arenas stay `Vec<T>` (array of structures).
- **The GPU toolchain**: rust-gpu (SPIR-V, shares the core) and Rust-CUDA (PTX, NVIDIA only) both need nightly and are incomplete. Hand-written WGSL or Metal shaders share no code and would be a second implementation tested against the CPU one.
- **`TextureEvaluator`**: returns with the wavefront driver (ADR 0006).
- **GPU-shareable `*Kind` enums**: rust-gpu cannot yet compile data-carrying enums, so a port would split each `*Kind` into a plain tag and handle.

## Considered options

- **Commit to nothing until a GPU port starts**: rejected. Allocation, recursion and `std` calls spread through per-sample code quickly and are expensive to remove later.
- **A `no_std` core that links `alloc`**: rejected. It would let per-sample code allocate, which ADR 0008 forbids and no GPU toolchain supports, and the counting-allocator test would be the only guard.
- **Panic-free core code** (`Option` or `get` on every access, `#![deny(clippy::panic)]`-style lints): rejected. It hurts readability on every line for a property a GPU port can supply by compiling bounds checks into traps.
- **`#[repr(C)]` + `Pod` on every core type now**: rejected. It forbids the `*Kind` enums and forces manual padding, for a layout no current code needs.
- **Constraining the CPU code to match a GPU bit for bit**: rejected. Nobody knows what a future GPU toolchain guarantees, so constraints chosen now would be guesses.

## Consequences

- ADR 0008's per-sample contract reads `SceneView<'_>` where it says `&Scene`.
- The per-thread context of typed buffers (ADR 0008) is owned outside the core, because it holds `Vec`s. The core receives `&mut [Vertex]` and other fixed-capacity slices, and tracks their length itself.
- Any new dependency of the core must build without `std`. That rules out many crates, and some, such as `wide`, need their default features turned off.
- A `no_std` crate cannot use `std`'s `f32` methods. Those methods are already banned by ADR 0002, so this changes nothing in practice: the core uses `libm`.
- Fixed-size stacks set hard limits, such as BVH depth. Exceeding one is a bug. BVH construction must make sure its trees fit the traversal stack.
