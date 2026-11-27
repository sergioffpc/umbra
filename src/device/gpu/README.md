# device/gpu

The GPU device, `umbra::device::gpu`, kept out of the umbra binary: umbra
links only the loader, and the backend is a separate module,
`libumbra-gpu.so`, loaded at run time when it is installed and a usable GPU
exists. It implements the `Device` interface of `umbra::device`, which never
depends on it.

Public headers (`include/umbra/device/gpu/`):

- `module.h`: the loader. `Module` loads the module with `dlopen` and calls
  it; `DescribeGpus` is what `umbra devices` lists, and `CreateDevice` makes
  the `device::Device` of a GPU, `gpu<ordinal>`, which commits and renders
  through the module. Its host thread takes a product's tiles from the
  counter in batches of `kTilesPerLaunch`, about one million pixels, and
  has the module render each batch in one launch and copy it into the
  render buffers before it takes the next. The module is looked up in
  `lib/` next to the `bin/` directory of the executable.
- `module_abi.h`: the table of functions the module exports through its one C
  symbol, `umbra_gpu_module`, with its ABI version. The loader hands the
  module umbra's log function through it, so the module logs as the `gpu`
  component without logging state of its own, and the module describes every
  GPU through it, usable or not. It also creates and destroys a device on a
  GPU, commits a change set to it, prepares the render of a product and
  renders a batch of its tiles: the change set crosses as the view of
  plain data that `umbra/device/change_set.h` describes, read only during the
  call. `DescribeGpus` adds what the loader reads
  from sysfs: each GPU's NUMA node and the kernel driver's version.

`backend/` is the module itself, built with `UMBRA_GPU` (see
`cmake/UmbraCuda.cmake`): host code compiled by clang, and device code
(`.cu`) compiled by nvcc to PTX and embedded into the module, which the
driver's OptiX compiles when a device first renders. It describes the GPUs
and which of them are usable (`sm_120` or newer, with a driver whose OptiX
it can use).

The host code calls the CUDA runtime (linked statically, `cudart_static`)
and OptiX only through their bridges, `umbra::ext::cuda` and
`umbra::ext::optix`, which the module links and hands its log function.
The programs call OptiX's device intrinsics directly
(`docs/adr/0017-ext-is-a-thin-layer-over-each-dependency.md`).

Each device the module creates has its own stream and OptiX context, and
holds in the GPU's memory, by id, what the commits gave it: the arrays of
each prototype (points as `float3` at a 16-byte stride, one array per motion
key), the parameters and placements of each light and camera, and the
acceleration structures built from them:

- one GAS per prototype, with one motion key per motion sample when it
  deforms within the shutter. `kTopology` rebuilds it; `kPoints` refits it,
  and rebuilds it after `kMaxRefits` refits in a row. It is built with
  `OPTIX_BUILD_FLAG_ALLOW_UPDATE` only once its points have changed, so that
  static geometry never pays for it;
- one GAS per light with a shape (sphere, rect, disk, cylinder): a single
  custom primitive that bounds the shape in the light's own space, built
  whenever the light is committed;
- one IAS, rebuilt at every commit, with a record per instance of each
  prototype and of each light with a shape, which points to its GAS: an
  instance that moves within the shutter sits behind an
  `OptixMatrixMotionTransform`.

Motion keys span the shutter as times 0 to 1. Each call makes the device's
GPU current on the calling thread, so that every device commits on a host
thread of its own. Of materials and textures, a device holds the ids that
prototypes and lights refer to.

A device renders with the placeholder integrator's OptiX pipeline
(`placeholder.cu`): a ray generation program that runs the single-source
camera model, pixel filter and sampler, one thread per pixel of a tile and
one tile per layer of a launch; a closest-hit program for triangles; and an
intersection and a closest-hit program for the lights' shapes, which read
each light's shape by its id. It traces graphs of any kind, with motion
blur, up to three levels deep. It writes the color, depth, normal and
albedo the CPU devices write. The module makes each product's camera
model on the host, from the device's copy of the camera, with the camera
code of `src/device`, which it builds in. At its first render a device
compiles the pipeline and uploads the sampler's cache; its film, in its
memory, grows to the largest product it renders.
