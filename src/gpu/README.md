# gpu

The GPU backend, kept out of the umbra binary: umbra links only the loader,
and the backend is a separate module, `libumbra-gpu.so`, loaded at run time
when it is installed and a usable GPU exists.

Public headers (`include/umbra/gpu/`):

- `module.h`: the loader. `Module` loads the module with `dlopen` and calls
  it; `DescribeGpus` is what `umbra devices` lists, and `CreateDevice` makes
  the `device::Device` of a GPU, `gpu<ordinal>`, which commits through the
  module. The module is looked up in `lib/` next to the `bin/` directory of
  the executable.
- `module_abi.h`: the table of functions the module exports through its one C
  symbol, `umbra_gpu_module`, with its ABI version. The loader hands the
  module umbra's log function through it, so the module logs as the `gpu`
  component without logging state of its own, and the module describes every
  GPU through it, usable or not. It also creates and destroys a device on a
  GPU, and commits a change set to it: the change set crosses as the view of
  plain data that `umbra/device/change_set.h` describes, read only during the
  call. `DescribeGpus` adds what the loader reads
  from sysfs: each GPU's NUMA node and the kernel driver's version.

`backend/` is the module itself, built with `UMBRA_GPU` (see
`cmake/UmbraCuda.cmake`): host code compiled by clang that calls the CUDA
runtime (linked statically, `cudart_static`) and OptiX, and device code
(`.cu`) compiled by nvcc to OptiX-IR and embedded into the module. It
describes the GPUs and which of them are usable (`sm_120` or newer, with a
driver whose OptiX it can use).

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
