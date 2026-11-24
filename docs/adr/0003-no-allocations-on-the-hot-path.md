# No allocations on the render hot path

The hot path is the work a render repeats for every sample: generating rays,
traversing the scene, shading, sampling lights and accumulating into the
image, on the CPU and on the GPU, and the per-frame loop around it. Nothing on
the hot path allocates memory: no `malloc`, no `new`, no container that grows,
no `std::string` or `std::format`, and on the GPU no device-side `malloc` and
no `cudaMalloc` between frames.

Memory is allocated before the hot path starts: when a scene is loaded or
changes (geometry, BVHs, materials, lights), and when a render starts
(the image, the GPU buffers, and each thread's scratch memory, sized from the
render settings such as the maximum path depth). The hot path reuses it.

An allocation costs an unpredictable time, may take a lock shared by every
thread, and fragments memory over a long render; a renderer that allocates
per sample runs as fast as its allocator lets it. Without allocations on the
hot path, the allocator only matters while loading, so umbra keeps the C
library's.

## Consequences

- Hot-path functions are declared `[[clang::nonallocating]]` (or
  `[[clang::nonblocking]]`, which also excludes locks), so that clang's
  function effect analysis reports at compile time a call that may allocate.
  The RealtimeSanitizer (`-fsanitize=realtime`) checks the same at run time,
  inside third-party libraries too. clang 23 and its runtime are in the
  toolchain.
- A library call on the hot path that allocates and cannot be avoided is an
  exception: it is marked where it is called (`__rtsan::ScopedDisabler`) and
  says why. The known one is OpenImageIO's texture cache on the CPU, which
  allocates a tile, under a lock, on each miss; a larger cache makes misses
  rarer but not absent. The GPU path uploads its textures before rendering
  and has no such exception.
- Diagnostics on the hot path are trace messages, which exist only in debug
  builds (docs/adr/0002-cli-diagnostics-on-stderr.md); a message that formats
  a string allocates.
- Errors on the hot path cannot be reported by building an `Error` with a
  message; they are recorded as values in preallocated memory and reported
  after the hot path ends.
