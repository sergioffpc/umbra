# umbra-gpu

The GPU's renderer, host side: the CUDA driver through `cudarc`, OptiX
through its function table, and the PTX of `umbra-gpu-programs`, which its
build script compiles and it embeds (ADR 0026, ADR 0028, ADR 0033). Its
`Renderer` renders a batch of Work Units at a time, up to the GPU Worker's
capacity, through the wavefront of `umbra-gpu-programs`: one launch per
stage over every Pixel Sample of the batch, with structure-of-arrays queues
between the stages, and one Work Result per Work Unit back. Its `Tracer` is
the tracer bullet of [#126](https://github.com/sergioffpc/umbra/issues/126):
one triangle and one sphere, traced through OptiX and read back on the host.

It is a `std` crate. It depends on `umbra-render` for the contract every
Render Device shares, on `umbra-scene`, `umbra-integrators`,
`umbra-sampling`, `umbra-spectrum`, `umbra-geometry` and `umbra-math`, on
`umbra-gpu-programs` for the layout it shares with the device, on `cudarc`
for the CUDA driver API and on `libloading` for OptiX (arc42 §5.1). The GPU
Local Worker will use it from `umbra-cluster`
([#128](https://github.com/sergioffpc/umbra/issues/128)).

```rust,no_run
use umbra_gpu::Renderer;
use umbra_render::{Cancellation, Rendered, WorkUnit};
use umbra_scene::{RenderJobOptions, SceneBuilder};

// pbrt-v4's default camera-only Scene, on the first GPU.
let scene = SceneBuilder::new().build()?;
let renderer = Renderer::new(0, &scene, &RenderJobOptions::default())?;

// One batch of up to `Renderer::CAPACITY` Work Units, one Rendered each.
let units: Vec<WorkUnit> = renderer.decomposition().work_units().take(16).collect();
for rendered in renderer.render(&units, &Cancellation::new())? {
    let Rendered::Done(result) = rendered else { unreachable!("not cancelled") };
    assert!(result.pixels().iter().all(|pixel| pixel.rgb == [0.0; 3]));
}
# Ok::<(), Box<dyn std::error::Error>>(())
```

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
| `renderer`  | `Renderer`: `new(ordinal, &Scene, &RenderJobOptions)`, `CAPACITY`, and `render(&[WorkUnit], &Cancellation) -> Result<Vec<Rendered>, Error>`         |
| `tracer`    | `Tracer`: the pipeline, one GAS per geometry, their shader binding tables, `trace`; and in `tracer/math`, `evaluate` for the GPU's math             |
| `device`    | What the two share: a GPU's stream, OptiX context and module of the programs, shader binding table records, and a launch that waits for its end     |
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

The `libm-bench` feature, off by default, makes the build script compile a
second PTX, of `umbra-gpu-programs`' libm bench kernels, for
`benches/libm.rs`: what each C math function costs on the GPU, the
counterpart of `umbra-math`'s bench of the same name, whose README says how
to run both.

Not here yet: the stages that trace rays and shade their hits, which
arrive with each feature on both Render Devices, and the GPU Local Worker
that pulls Work Units for the `Renderer`
([#128](https://github.com/sergioffpc/umbra/issues/128)).

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

### A batch of Work Units, one launch per stage

A GPU Worker keeps `Renderer::CAPACITY` Work Units in flight, 16 to start
with and to be tuned by benchmark, and `render` takes them as one batch
(ADR 0026): Tile size and Pixel Samples per Pass stay those of every
Worker. Each stage is a ray-generation program launched once with one
thread per Pixel Sample of the batch, Work Unit after Work Unit, pixel by
pixel, sample index by sample index; `WorkUnitRecord` tells a thread which
Pixel Sample it is. The stages pass each Pixel Sample on through a queue of
one array per field (`PixelSampleQueue`), and the film stage writes what
it adds to its pixel. The host sums those in `f64`, pixel by pixel, as
`PixelSum` sums them on the CPU, so a Work Result differs from the CPU's
only where the GPU rounds differently (ADR 0026). Cancellation is checked
before the batch and between stages; a cancelled batch returns
`Rendered::Cancelled` for every Work Unit.

### The Scene's view uploaded once, its tables by `map_storage`

`Renderer::new` uploads what the stages read for the whole Render Job: a
`SceneRecord`, the `SceneView`'s camera, filter and sensor with the
integrator, and a `SamplerRecord`, the sampler with its digit permutations.
The filter's and the sensor's tables, and the permutations, are generic over
their storage in the core; the host gathers them into one buffer each with
`map_storage`, uploads it, and maps each table's place in it to a
`DeviceSlice`, which the record holds instead of a host reference (arc42
§8.12). On the device, `DeviceSlice` is `AsRef<[T]>`, so the core samples
the tables there as it samples a `Vec` on the CPU.

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

### One shader binding table per geometry, one per stage

The triangle's hit group record carries no data and the sphere's carries its
`Sphere`, so the two have different strides. Each launch traces one GAS
directly, with a table that holds that geometry's one hit group record and
the shared ray-generation and miss records. The `Renderer` has one table per
stage, its ray-generation record that stage's; no stage traces a ray yet,
but every table holds a miss and a hit group record.

### OptiX objects own what they were created from

A module holds an `Rc` of its context, a program group of its module, a
pipeline of its context and groups, so each is destroyed after everything
created from it, whatever order `Tracer`'s or `Renderer`'s fields drop in.
They stay on the thread that created them, so neither is `Send`.

## Things to know when using it

- `Renderer::new` and `Tracer::new` need an NVIDIA driver new enough for
  OptiX 9.0; in a container, `NVIDIA_DRIVER_CAPABILITIES=compute,graphics`
  (arc42 §7).
- `render` panics if a Work Unit is not one of the Render Job's, as
  `umbra-cpu`'s does. It may be given more than `CAPACITY` Work Units: the
  capacity is what a GPU Worker asks the Coordinator for, and the batch's
  memory grows with it.
- The PTX runs only if the crate was built with `libdevice.10.bc`, found at
  `UMBRA_LIBDEVICE`, else under `CUDA_PATH`, `CUDA_HOME` or `/usr/local/cuda`
  (`nvvm/libdevice/libdevice.10.bc`). Without it the build warns, and
  `Renderer::new` and `Tracer::new` return `Error::UnlinkedPtx`.
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
`--no-capture` prints the measured errors. `tests/gpu/renderer.rs` parses
the camera-only Scene File of `tests/gpu/scenes/`, renders it in batches of
`Renderer::CAPACITY` on the GPU and Work Unit by Work Unit on the CPU, and
checks one black Work Result per Work Unit whose pixels' filter weights are
the CPU's to `f32` rounding: the GPU draws the same Pixel Samples, and with
no radiance the noise is no larger than the rounding. They need a GPU, so they are the test binary
`gpu`, which is their label: nextest's `default` and `ci` profiles leave out
every binary of that name, and `cargo nextest run --profile gpu` runs them
alone, on the P620 (`.config/nextest.toml`).
