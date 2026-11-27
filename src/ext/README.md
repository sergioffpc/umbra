# ext

Bridges to third-party libraries that need RTTI or exceptions. Each bridge is
the module `umbra::ext::<dependency>`, with its sources in
`src/ext/<dependency>/` and its public headers in
`include/umbra/ext/<dependency>/`, defined with `umbra_add_ext_module`.

The OpenUSD bridge, `umbra::ext::usd`, initializes OpenUSD and its plugins,
routes Tf's diagnostics into the log, and opens stages for the render
delegate, `umbra::delegate` (`src/delegate/`).

The Embree bridge, `umbra::ext::embree`, creates the process's Embree
device and routes its errors into the log (`embree.h`), owns its scenes and
geometries (`owners.h`), turns a failed build into a `core::Error`
(`errors.h`), converts between Embree's types and umbra's
(`conversions.h`), and redeclares nonblocking the functions the CPU
device's hot path calls (`nonblocking.h`). The CPU device,
`umbra::device::cpu` (`src/device/cpu/`), builds its BVHs with them.

The CUDA and OptiX bridges, `umbra::ext::cuda` and `umbra::ext::optix`, are
linked only into the GPU module (`src/device/gpu/backend/`), which links no
umbra module: they are built position-independent with
`umbra_add_ext_module(... LOADABLE ...)`, see core's headers without
linking it, return their errors as messages, as the module does, and log
what they cannot return through the function the module hands them
(`SetLog`). The CUDA bridge owns GPU memory and streams (`owners.h`),
translates the runtime's errors (`errors.h`), wraps the runtime calls the
module makes (`runtime.h`) and converts a driver's device pointer into the
runtime's (`conversions.h`). The OptiX bridge loads the driver's OptiX
runtime and defines its function table (`optix.h`), translates its errors
(`errors.h`), owns its context, modules, program groups and pipelines
(`owners.h`), routes the context's messages and those of its compiles into
the log, and wraps the host calls the module makes (`host.h`).

Rules:

- Bridges and the render delegate (`src/delegate/`) are the only targets
  built with `-frtti` and `-fexceptions`.
- Bridges and the render delegate are the only targets with default
  visibility in Release: the library's classes live in its shared objects,
  which hold subclasses outside umbra's LTO unit and the type_info that RTTI
  compares, so LTO must not devirtualize their calls nor umbra keep its own
  copy of their type_info.
- A bridge is the only code that includes the headers of its library,
  except OpenUSD's, which the render delegate includes too, and Embree's,
  which the CPU device includes too.
- Its public headers expose only umbra types, never the library's, except
  the OpenUSD bridge's `stage.h`, which hands the render delegate the stage
  it opens, the Embree bridge's `owners.h`, `errors.h`,
  `conversions.h` and `nonblocking.h`, which the CPU device builds and
  traces its BVHs with, and the CUDA and OptiX bridges' headers, which take
  and hand the GPU module the types of CUDA and OptiX.
- Every exception the library may throw is caught inside the bridge and
  returned as `std::expected` with the core error type, except by the CUDA
  and OptiX bridges, whose libraries throw nothing and whose errors are
  messages, as those of the GPU module they are linked into.
