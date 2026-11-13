# umbra-gpu

The GPU's renderer, host side: the CUDA driver through `cudarc`, OptiX
through its function table, and the PTX of `umbra-gpu-programs`, which its
build script compiles and it embeds (ADR 0026, ADR 0028, ADR 0033). Its
`Renderer` has the shape of `umbra-cpu`'s, a Work Unit in and its Work
Result out, but renders nothing yet: `Renderer::new` refuses every Scene
until the wavefront arrives. What runs today is the tracer bullet of
[#126](https://github.com/sergioffpc/umbra/issues/126): one triangle and one
sphere, traced through OptiX and read back on the host.

It is a `std` crate. It depends on `umbra-render` for the contract every
Render Device shares, on `umbra-scene`, `umbra-geometry` and `umbra-math`,
on `umbra-gpu-programs` for the layout it shares with the device, on
`cudarc` for the CUDA driver API and on `libloading` for OptiX;
`umbra-cluster` depends on it (arc42 §5.1).

```rust,no_run
use umbra_geometry::{Ray, Sphere};
use umbra_gpu::{Geometry, Tracer};
use umbra_math::{Point3f, Vector3f, translate};

let triangle = [
    Point3f::new(-1.0, -1.0, 5.0),
    Point3f::new(1.0, -1.0, 5.0),
    Point3f::new(0.0, 1.0, 5.0),
];
let sphere = Sphere::new(translate(Vector3f::new(0.0, 0.0, 10.0)), 2.0);
let tracer = Tracer::new(0, triangle, &sphere)?;

let ray = Ray::new(Point3f::new(0.0, 0.0, 0.0), Vector3f::new(0.0, 0.0, 1.0));
let hits = tracer.trace(Geometry::Sphere, &[ray])?;
assert_eq!(hits[0].map(|hit| hit.t), Some(8.0));
# Ok::<(), umbra_gpu::Error>(())
```

## What is in it

| Module      | Contents                                                                                                                                            |
| ----------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| `renderer`  | `Renderer`: `new(&Scene, &RenderJobOptions)` and `render(WorkUnit, &Cancellation) -> Rendered`, as `umbra-cpu`'s; `new` refuses every Scene yet     |
| `tracer`    | `Tracer`: the pipeline, one GAS per geometry, their shader binding tables, `trace`; and in `tracer/math`, `evaluate` for the GPU's math             |
| `optix`     | OptiX loaded from the driver, and an owning wrapper per object: context, module, program groups, pipeline; GAS builds and launches                  |
| `optix/ffi` | OptiX 9.0's host ABI (105): the function table and the structs and constants Umbra's calls take, with their layout asserted                         |
| `records`   | Shader binding table records, `DeviceCopy`, which carries `cudarc`'s `DeviceRepr` for `umbra-gpu-programs`' layout, and the hits a hit record holds |
| `error`     | `Error`                                                                                                                                             |
| `build.rs`  | Compiles `umbra-gpu-programs` for `nvptx64` as a `cdylib`, in a target directory of its own, with `libdevice` linked in                             |

`Geometry`, `Hit`, `Renderer`, `Tracer` and `Error` are public at the crate
root.

The `math-program` feature, off by default, adds the math program to the
PTX and the pipeline, `Tracer::evaluate`, and `MathFunction` and
`UnknownMathFunction` at the crate root: for the GPU tests alone, which turn
it on by a dev-dependency of the crate on itself (#146).

Not here yet: the `SceneView` upload, the wavefront queues and a GPU
`Renderer` that renders, which arrive with the features on both Render Devices
([#127](https://github.com/sergioffpc/umbra/issues/127)) and the GPU Local
Worker ([#128](https://github.com/sergioffpc/umbra/issues/128)).

## Design decisions

### Nothing from NVIDIA at build time

`cudarc` loads `libcuda.so.1` and `libloading` loads `libnvoptix.so.1` at run
time, both from the NVIDIA driver, so the crate builds on machines without a
GPU or CUDA, CI included, and the release image ships nothing from NVIDIA
(ADR 0026). The OptiX bindings are written by hand from OptiX 9.0's ABI, with
no SDK header copied (NOTICE): only the slots of the function table Umbra
calls are typed, and the struct members it leaves unset are zeroed arrays of
the same size, each struct's layout asserted.

### The layout by an ordinary dependency

The launch parameters, the ray and hit records and the math program's
parameters are defined once, in `umbra-gpu-programs`, which compiles for the
host as well as for `nvptx64`; this crate depends on it for those types
alone (ADR 0033). Their types are that crate's, so `cudarc`'s `DeviceRepr`
cannot be implemented on them here: `records::DeviceCopy` wraps each,
`#[repr(transparent)]`, and carries the promise instead.

### The programs built by a nested `cargo`

`build.rs` runs `cargo rustc --lib --crate-type cdylib` on
`umbra-gpu-programs` for `nvptx64-nvidia-cuda`, whose manifest makes its
library an `rlib` for the dependency above,
in release and without debug information, into a target directory under
`OUT_DIR`, with the outer build's `RUSTFLAGS` and wrappers removed: they are
for the host. It links CUDA's `libdevice.10.bc` with `-C link-arg`, which the
toolchain's `llvm-bitcode-linker` inlines, so the PTX keeps no `.extern`
(ADR 0028, [#146](https://github.com/sergioffpc/umbra/issues/146)). It reruns
when a source of the programs' build changes, as their dep-info lists them.
Every host build thereby also checks that the core builds without `std`.

### One shader binding table per geometry

The triangle's hit group record carries no data and the sphere's carries its
`Sphere`, so the two have different strides. Each launch traces one GAS
directly, with a table that holds that geometry's one hit group record and
the shared ray-generation and miss records.

### OptiX objects own what they were created from

A module holds an `Rc` of its context, a program group of its module, a
pipeline of its context and groups, so each is destroyed after everything
created from it, whatever order `Tracer`'s fields drop in. They stay on the
thread that created them.

## Things to know when using it

- `Tracer::new` needs an NVIDIA driver new enough for OptiX 9.0; in a
  container, `NVIDIA_DRIVER_CAPABILITIES=compute,graphics` (arc42 §7).
- The PTX runs only if the crate was built with `libdevice.10.bc`, found at
  `UMBRA_LIBDEVICE`, else under `CUDA_PATH`, `CUDA_HOME` or `/usr/local/cuda`
  (`nvvm/libdevice/libdevice.10.bc`). Without it the build warns, and
  `Tracer::new` returns `Error::UnlinkedPtx`.
- In builds with debug assertions, OptiX's validation mode is on: slower, but
  it reports misuse.

## Tests

`tests/gpu/optix.rs` traces fixed rays through OptiX: against the
triangle, whose hits are worked by hand, and against the sphere, a few
chosen rays and a fan of 1024, each compared with the core's
`Sphere::intersect` on the CPU. `tests/gpu/libdevice.rs` evaluates, with
`Tracer::evaluate`, every `libdevice` function that `umbra-math`'s `nvptx64`
backend calls on millions of arguments across the `f32` range, subnormals
included, and checks each against glibc on the CPU within the bound
`umbra-math`'s `src/libdevice.rs` documents; that the IEEE primitives keep
subnormals shows that `__nvvm_reflect("__CUDA_FTZ")` resolved to 0 (#146).
`--no-capture` prints the measured errors. They need a GPU, so they are the test binary
`gpu`, which is their label: nextest's `default` and `ci` profiles leave out
every binary of that name, and `cargo nextest run --profile gpu` runs them
alone, on the P620 (`.config/nextest.toml`).
