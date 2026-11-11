# umbra-kernels

The GPU's programs: the OptiX entry points that run the core on the GPU, the
OptiX device API they call, in inline PTX, and the panic handler that traps
(ADR 0026, ADR 0028).

It is `#![no_std]` and built for `nvptx64-nvidia-cuda` only: on any other
target the crate is empty, so a host build of the workspace compiles it
without the GPU's toolchain. It uses the core crates, `umbra-geometry` and
`umbra-math` so far, and nothing depends on it: `umbra-gpu`'s build script
compiles it to PTX and embeds that (arc42 §5.1). It is the only crate that
enables unstable features (ADR 0027): `abi_ptx` for the `ptx-kernel` entry
points and `asm_experimental_arch` for inline PTX.

There is no example: nothing in it runs on the host. `umbra-gpu`'s `Tracer`
is the way to run it.

## What is in it

| Module   | Contents                                                                                                                                   |
| -------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| `lib.rs` | The four programs of the tracer bullet, the `.const` launch parameters `params`, and the panic handler                                     |
| `optix`  | The OptiX 9.0 device API: one safe wrapper per `_optix_*` built-in (launch index, payload, ray, hit kind, SBT data, report, trace, `trap`) |
| `launch` | The layout the host shares: `LaunchParams`, `RayRecord`, `HitRecord` and `HitKind`; `umbra-gpu` compiles the same file by `#[path]`        |

The programs, for one triangle and one sphere traced from the host
([#126](https://github.com/sergioffpc/umbra/issues/126)):

| Entry point              | What it does                                                                                        |
| ------------------------ | --------------------------------------------------------------------------------------------------- |
| `__raygen__trace`        | Traces ray `i` of the launch parameters through their traversable and writes hit record `i`         |
| `__miss__trace`          | Marks the ray as a miss                                                                             |
| `__closesthit__trace`    | Returns the hit's distance, and whether a triangle or the sphere was hit                            |
| `__intersection__sphere` | Intersects the ray with the `Sphere` of its hit group record through the core's `Sphere::intersect` |

Not here yet: the wavefront kernels of the integrators, which arrive with
each feature on both Render Devices
([#127](https://github.com/sergioffpc/umbra/issues/127)).

## Design decisions

### Upstream `rustc`, at the target's default GPU

The PTX comes from upstream `rustc`'s NVPTX backend (ADR 0028), at the
target's default, `sm_70` and PTX ISA 7.0: the driver compiles that PTX for
any newer GPU, the P620's Blackwell included. `-C target-cpu` is not set, as
the pinned nightly treats it as part of the ABI and would refuse to link the
prebuilt `core` built without it.

### OptiX's device API as hand-written inline PTX

Each OptiX device function is a `call` to an `_optix_*` built-in, which
OptiX resolves when it compiles the module, the same PTX the CUDA headers
produce. The built-ins' names and operands are facts of OptiX's ABI, written
by hand: no header text is copied, since most of the OptiX SDK's headers are
under NVIDIA's proprietary license (ADR 0026, NOTICE). Each wrapper is
`unsafe` inside and safe outside, one built-in per block (ADR 0007).

### Launch parameters in a `.const` variable

OptiX passes a launch's parameters in a `.const` variable the pipeline names.
`rustc` cannot place a static in the constant state space, so `params` is
declared with `global_asm!` and read field by field with `ld.const`, at the
offsets `launch.rs` asserts.

### Panics trap

A panic is a bug (ADR 0014): the handler executes `trap`, which aborts the
launch, and the host sees a CUDA error.

## Things to know when using it

- The programs read device memory by the addresses the host passes: the
  host must size the ray and hit buffers to `LaunchParams::count` and give
  the sphere's hit group record a `Sphere` (`umbra-gpu` does both).
- The pipeline must name `params` as its launch-parameter variable and use
  two payload values, which `umbra-gpu`'s compile options do.

## Tests

None run on the host. `umbra-gpu`'s tests run these programs on the P620
(`scripts/gpu-test`), and `scripts/check.sh` builds the crate for `nvptx64`
and runs clippy on it.
