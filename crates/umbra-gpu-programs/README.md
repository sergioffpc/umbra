# umbra-gpu-programs

The GPU's renderer, device side: the OptiX entry points that run the core on
the GPU, the OptiX device API they call, in inline PTX, and the panic handler
that traps (ADR 0026, ADR 0028), with the layout they share with the host
(ADR 0033).

It compiles for the host and for `nvptx64-nvidia-cuda`. On both it holds the
shared layout: `LaunchParams`, `RayRecord`, `HitRecord` and `HitKind`, and
with the `math-program` feature `MathParams` and `MathFunction`. On
`nvptx64` only it is `#![no_std]` and holds the programs, `optix`, `math`
and the panic handler, so a host build of the workspace compiles the layout
alone, without the GPU's toolchain. It uses the core crates,
`umbra-geometry` and, on `nvptx64`, `umbra-math`. `umbra-gpu` depends on it
for the layout, and its build script compiles it to PTX and embeds that
(arc42 §5.1). It is the only crate that enables unstable features, on
`nvptx64` only (ADR 0027): `abi_ptx` for the `ptx-kernel` entry points and
`asm_experimental_arch` for inline PTX.

There is no example: none of its programs runs on the host. `umbra-gpu`'s
`Tracer` is the way to run them.

## What is in it

| Module        | Contents                                                                                                                                                 |
| ------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `programs`    | On `nvptx64`: the four programs of the tracer bullet and the math program, the `.const` launch parameters `params`, and the panic handler                |
| `math`        | On `nvptx64`, with `math-program`: what the math program runs, its function of `umbra-math`, and its launch parameters, arguments and results            |
| `optix`       | On `nvptx64`: the OptiX 9.0 device API, one safe wrapper per `_optix_*` built-in (launch index, payload, ray, hit kind, SBT data, report, trace, `trap`) |
| `launch`      | On both targets: the layout the host shares, `LaunchParams`, `RayRecord` (from the core's `Ray`), `HitRecord` and `HitKind`                              |
| `math_launch` | On both targets, with `math-program`: the math program's layout the host shares, `MathParams`, `MathFunction` and its ids, `UnknownMathFunction`         |
| `libm_bench`  | On `nvptx64`, under `--cfg umbra_libm_bench` only: the plain CUDA kernels of `umbra-gpu`'s libm bench, and their panic handler                           |

The layout's types are public at the crate root.

The programs, for one triangle and one sphere traced from the host
([#126](https://github.com/sergioffpc/umbra/issues/126)):

| Entry point              | What it does                                                                                        |
| ------------------------ | --------------------------------------------------------------------------------------------------- |
| `__raygen__trace`        | Traces ray `i` of the launch parameters through their traversable and writes hit record `i`         |
| `__miss__trace`          | Marks the ray as a miss                                                                             |
| `__closesthit__trace`    | Returns the hit's distance, and whether a triangle or the sphere was hit                            |
| `__intersection__sphere` | Intersects the ray with the `Sphere` of its hit group record through the core's `Sphere::intersect` |
| `__raygen__math`         | Evaluates the launch's `MathFunction` on argument record `i` and writes result `i` (#146)           |

Not here yet: the wavefront stages of the integrators, which arrive with
each feature on both Render Devices
([#127](https://github.com/sergioffpc/umbra/issues/127)).

## Design decisions

### One crate, two targets

The layout is defined once and compiled on both sides, so the host and the
device cannot disagree on it; `umbra-gpu` uses it through an ordinary
dependency (ADR 0033). The library is an `rlib` for that dependency;
`umbra-gpu`'s build script asks for a `cdylib` with `cargo rustc
--crate-type cdylib`, which the `nvptx64` target links into one PTX module.
No build script compiles its own crate.

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

### The GPU's math, run as a launch

The checks of `umbra-math`'s `nvptx64` backend against glibc
([#146](https://github.com/sergioffpc/umbra/issues/146)) need the GPU to
evaluate each `libdevice` function on chosen arguments. A second
ray-generation program does it in the tracer's own pipeline: a math launch
puts `MathParams` in the same `params` variable, which is sized for the
larger `LaunchParams`, and selects `__raygen__math` by its shader binding
table record. It traces no ray. It, `math` and `math_launch` exist only
with the `math-program` feature, which `umbra-gpu`'s feature of the same
name turns on, in the programs' build and in the layout it uses; production
PTX has no math program.

### The libm bench's kernels

`umbra-gpu`'s libm bench times each C math function three ways on the GPU:
`libdevice`, `umbra-math`'s `nvptx64` backend and the approximate
intrinsics. Its kernels are plain CUDA kernels, and the CUDA module that
runs them cannot load the OptiX programs. So
`umbra-gpu`'s build script, under its `libm-bench` feature, builds this
crate a second time into a PTX of its own, with `--cfg umbra_libm_bench`,
which compiles `libm_bench` and leaves `programs`, `optix` and `math` out.
A cfg rather than a feature, since it takes modules out: the production
PTX is built without it.

### Panics trap

A panic is a bug (ADR 0014): the handler executes `trap`, which aborts the
launch, and the host sees a CUDA error.

## Things to know when using it

- The programs read device memory by the addresses the host passes: the
  host must size the ray and hit buffers to `LaunchParams::count`, the
  argument and result buffers to `MathParams::count`, and give the sphere's
  hit group record a `Sphere` (`umbra-gpu` does all three).
- The pipeline must name `params` as its launch-parameter variable and use
  two payload values, which `umbra-gpu`'s compile options do.

## Tests

None run on the host. `umbra-gpu`'s tests run these programs on the P620
(`cargo nextest run --profile gpu`), and `scripts/check.sh` builds the crate
for `nvptx64` and runs clippy on it.
