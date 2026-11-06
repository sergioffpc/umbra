# Index-handle arenas and allocation-free rendering

The Scene stores every object in a per-type arena (one `Vec<T>` per concrete type) and objects refer to one another through typed `u32` index handles (`MeshId`, `MaterialId`, generically `Handle<T>`), never through references, `Box`, `Arc` or a raw `usize`. Rendering code never allocates per Pixel Sample. Together these keep the per-sample code free of lifetimes, pointers and allocation, which pbrt-v4 achieves on the GPU with pointers into pre-allocated memory and which Rust gets more simply from indices ([pbrt-v4 CPU/GPU shared-code architecture](https://github.com/sergioffpc/umbra/issues/15)).

- **Tag plus handle.** Every Scene-resident `*Kind` enum (`ShapeKind`, `PrimitiveKind`, `MaterialKind`, `LightKind`, `FloatTextureKind`, `SpectrumTextureKind`, `MediumKind`) holds only handles and small scalars in its variants, like pbrt-v4's `TaggedPointer`. Each is bounded by a compile-time `size_of::<…>() <= 16` assertion. `ShapeKind::Triangle { mesh: MeshId, index: u32 }` is inline because it already is the whole pbrt-v4 `Triangle`. Moving a larger payload inline is an aggressive optimisation. `BxDFKind` stays by value (≤ 256 B, ADR 0006), and `SamplerKind`, `CameraKind` and `FilterKind`, one per Render Job, are held by value without a bound.
- **Built once, then immutable.** A mutable `SceneBuilder` fills the arenas and is the only place handles are minted. It freezes into an immutable `Scene` with its BVH built. Validation rejects a Scene that would exceed `u32::MAX` objects of one type. Rendering threads share `&Scene`, with no `Arc`, lock or global mutable state. The Scene lives exactly one Render Job and is dropped all at once.
- **No scratch arena.** pbrt-v4's per-thread `ScratchBuffer` is replaced by a per-thread context of typed buffers (`Vec<Vertex>` for BDPT and MLT subpaths, …). Their capacity is set at Render Job start from `maxDepth` and the options, and they are cleared, never reallocated, between Pixel Samples.
- **Allocation boundary.** Heap allocation is allowed per Render Job (Scene, BVH, thread contexts), per Work Unit (the Work Result buffer) and outside rendering (parsing, network, image output), never per Pixel Sample. Recording a Pixel Trace is exempt. An integration test with a counting global allocator renders a Work Unit with N and 2N Pixel Samples and asserts the same allocation count. Every integrator joins that test.
- **Per-sample contract.** Per-Pixel-Sample code receives a `SceneView` of the Scene's arenas (reached through `get(handle)` or as slices `&[T]`), `&mut` slices of its thread context, and `Copy` values. It does not depend on `alloc`, so it fits the `no_std` core ([ADR 0014](0014-gpu-ready-no-std-rendering-core.md) replaced `&Scene` with `SceneView`). Handles are resolved with bounds-checked indexing. `get_unchecked` falls under ADR 0007.
- **Memory placement.** Per-thread mutable data that sits next to other threads' data is wrapped in `crossbeam_utils::CachePadded` (128 B on aarch64 and x86-64). Scene arenas keep natural alignment, and a cache-line BVH node layout is an aggressive optimisation of the BVH. NUMA-local placement is not done yet. Because the Scene holds no pointers and is `Clone`, a replica per NUMA node is a later, benchmark-driven change. The global allocator is the system one, and replacing it is an aggressive optimisation measured on the scene-build phase.

## Considered options

- **References into a bump arena (`&'scene T`)**: rejected. The lifetime spreads through every signature, the data is not position-independent, and it cannot move to a GPU or be cloned per NUMA node.
- **`Arc<T>` between objects**: rejected. It puts reference counting on the hot path and is neither `Copy` nor `no_std`.
- **Payloads inline in the `*Kind` enums**: rejected as the default. Each enum would be as large as its largest variant, so millions of triangles would pay for a sphere.
- **pbrt-v4's `ScratchBuffer`**: rejected. It needs `unsafe` or `bumpalo` and lifetimes tied to a reset, while the remaining per-sample buffers have sizes known at Render Job start.
- **A Worker process per NUMA node**: rejected. A Worker is one process per machine.

## Consequences

- Every object access through a handle costs one indexed load and one bounds check.
- A handle is meaningful only for the Scene that minted it. Mixing handles from two Scenes is not caught by the type system.
- Allocation-freedom is a tested property, not a convention, so a new integrator that allocates per Pixel Sample fails CI.
