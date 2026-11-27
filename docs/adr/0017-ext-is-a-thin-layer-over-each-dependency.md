# ext/ is a thin layer over each dependency

`src/ext/` holds umbra's bridges to third-party libraries. Over time they
also took in umbra's own logic. The render delegate grew inside the OpenUSD
bridge, the CPU device inside the Embree bridge, and the denoise inside the
OIDN bridge. This decision draws the line again: a bridge makes one
dependency follow umbra's rules and holds nothing else. umbra's logic lives
in modules of its own: the render delegate, and the devices behind their
interface.

## What a bridge holds

`src/ext/<dep>`, the module `umbra::ext::<dep>`, makes one dependency follow
the project's rules: no exceptions, errors as `std::expected`, diagnostics
in umbra's log, and no allocation or lock on the hot path. It holds only:

- RAII owners of the dependency's handles;
- translation of the dependency's errors, exceptions and callbacks into
  `core::Error`, `std::expected` and umbra's log;
- redeclarations, with their effects (`[[clang::nonblocking]]`), of the
  dependency's functions that the hot path calls;
- conversions between the dependency's types and umbra's plain types.

A bridge decides nothing about how umbra renders. It has no algorithm, no
policy and no choice of device. Code that decides lives in the module that
uses the bridge.

## Who calls what

Code outside `ext/` calls the bridges, never a dependency's own API. Three
exceptions are declared:

- **The render delegate may include Hydra's headers.** It derives from
  Hydra's base classes (`HdRenderDelegate`, its prims, `HdRenderPass`,
  `HdRenderBuffer`) and reads the scene delegate through them. Hydra does
  not throw through these calls. Anything that can fail on OpenUSD's side
  goes through the OpenUSD bridge: initialization, plugins, opening the
  stage, and Tf's diagnostics.
- **The CPU device may call Embree's API.** It builds its BVHs and traces
  its rays through Embree's C API, which neither throws nor allocates on
  the hot path, so wrapping each call would add nothing but a layer to
  pass through. It holds Embree's handles through the bridge's owners,
  reports its errors through the bridge's translation, and calls on the
  hot path only the functions the bridge redeclares nonblocking.
- **The GPU device's programs may call OptiX's device intrinsics**
  (`optixTrace`, `optixGetPayload_*` and the like). They neither throw nor
  allocate, and wrapping each of them would add nothing but a layer to
  pass through. The GPU's host code calls CUDA and OptiX through their
  bridges.

The rule holds by convention: CONTRIBUTING.md states it, and review checks
it. No tool enforces it. Every dependency installs under one prefix, so the
build cannot hide one library's headers from a module that is not its
bridge.

## Modules

Each module's namespace follows its path.

| Path | Namespace | What it holds |
|---|---|---|
| `src/delegate` | `umbra::delegate` | Hydra's render delegate: its prims, host copies, commit, render task, pass and buffers; the denoise, IES profiles and subdivision refinement; the API a render opens: `OpenSession`, `Session`, `Display` |
| `src/device` | `umbra::device` | the `Device` interface, the tile queue, the frame parameters and the single-source code |
| `src/device/cpu` | `umbra::device::cpu` | the CPU device: its arenas, NUMA placement, BVHs and tiles |
| `src/device/gpu` | `umbra::device::gpu` | the GPU device and the OptiX module it loads at run time |
| `src/ext/<dep>` | `umbra::ext::<dep>` | the bridge over `<dep>` |

The devices depend on the interface, never the reverse. The render
delegate is built as the bridges are, with RTTI and exceptions, because it
derives from Hydra's classes. It catches every exception at its edge, as a
bridge does, and exposes only umbra's types.

## Migration

The modules move one boundary at a time. Each move is mostly a move of
files, with no change of behaviour, so that review sees only the move.
They go in this order:

1. the render delegate, out of the OpenUSD bridge;
2. the CPU device, out of the Embree bridge;
3. the GPU device, over new CUDA and OptiX bridges;
4. the denoise, out of the OIDN bridge and into the delegate.

## Considered options

- **The delegate in `ext/usd`, as an exception to the rule.** It is most of
  the code under `ext/` and all of it is umbra's logic, so the rule would
  describe almost nothing.
- **A scene module with no OpenUSD types, behind a minimal Hydra layer.**
  It would move the sync out of Hydra's prims, which is where Hydra hands
  over what changed. That is much work, and no device or test needs it.
- **Wrapping Hydra's base classes, Embree's API and OptiX's intrinsics in
  the bridges.** The wrappers would only pass calls through, in hot code on
  both the CPU's side and the GPU's.
- **Enforcing the include rule with a check, or with one install prefix per
  dependency.** Review is enough for a rule this small. A separate prefix
  per dependency would mean changing the deps layer and every
  `find_package`.
- **One migration in one change.** It would be faster but hard to review,
  and it would conflict with every change in flight.

## Consequences

- Reading a bridge shows what umbra demands of that dependency, and
  nothing about how umbra renders.
- umbra's logic lives where its concept lives. The denoise and the
  subdivision refinement are steps of the delegate's work, and the BVH is
  the CPU device's.
- The render delegate is a second module built with RTTI and exceptions,
  besides the bridges.
- Nothing stops a module from including a dependency's headers but review.
