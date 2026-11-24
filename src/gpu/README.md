# gpu

The GPU backend, kept out of the umbra binary: umbra links only the loader,
and the backend is a separate module, `libumbra-gpu.so`, loaded at run time
when it is installed and a usable GPU exists.

Public headers (`include/umbra/gpu/`):

- `module.h`: the loader. `Module` loads the module with `dlopen` and calls
  it; `CountGpuExecutors` is what umbra reports. The module is looked up in
  `lib/` next to the `bin/` directory of the executable.
- `module_abi.h`: the table of functions the module exports through its one C
  symbol, `umbra_gpu_module`, with its ABI version. The loader hands the
  module umbra's log function through it, so the module logs as the `gpu`
  component without logging state of its own.

`backend/` is the module itself, built with `UMBRA_GPU` (see
`cmake/UmbraCuda.cmake`): host code compiled by clang that calls the CUDA
runtime (linked statically, `cudart_static`) and OptiX, and device code
(`.cu`) compiled by nvcc to OptiX-IR and embedded into the module. For now it
finds the usable GPUs (`sm_120` or newer, with a driver whose OptiX it can
use); the GPU executors will live there.
