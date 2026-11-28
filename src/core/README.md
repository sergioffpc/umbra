# core

The foundation every other module depends on: what umbra is and how it was
built, how errors are reported, and the per-thread and arithmetic rules all
code follows.

Public headers (`include/umbra/core/`):

- `version.h`: the umbra version, from the CMake project.
- `build_info.h`: the commit (with `-dirty`), build type, target architecture
  and compiler of the binary, generated on every build.
- `error.h`: the `[[nodiscard]]` error type, returned as
  `std::expected<T, Error>`.
- `log.h`: the diagnostics on stderr, filtered by level per component, and
  the `UMBRA_LOG_*` macros (see `docs/adr/0002-cli-diagnostics-on-stderr.md`).
- `log_hook.h`: a log through a function handed over at run time, and the
  threshold it applies, header-only: the GPU module and the bridges linked
  into it, which link no umbra module, log through umbra's diagnostics with
  it.
- `handle_owner.h`: the owner of a library's handle, which releases it with
  the library's function, header-only for the same bridges.
- `lazy_format.h`: a format string and its arguments, formatted only when
  asked, header-only for the same bridges: they take the message of a
  call's error this way, so that a call that succeeds formats nothing.
- `progress.h`: the progress of each phase of umbra's work on stderr, in
  place on a terminal and at every 10% otherwise, counted from any thread
  without allocating or locking.
- `number_format.h`: the durations, rates, sizes and counts umbra shows on
  stderr, whatever the locale.
- `cpu_topology.h`: the CPUs of each NUMA node, read from sysfs (model,
  cores, threads, memory, caches with the cores grouped by cache sizes), and
  the CPU's widest vector ISA. A CPU render device is one NUMA node (see
  `docs/adr/0004-render-devices.md`). It also parses a device's sysfs
  `numa_node` text, which the GPU loader reads for each GPU.
- `memory_policy.h`: a thread's NUMA memory policy, bound to a node and put
  back, and the node a page lies on, through the kernel's system calls. A
  CPU device binds the threads of its arena to its node (see
  `docs/adr/0013-devices-render-from-their-own-memory.md`).
- `wrapping.h`: wrapping add, sub, mul and shl, the only intentional
  overflow.
- `floating_point.h`: the per-thread FTZ/DAZ configuration, called first on
  every thread.
- `safety.h`: the `UMBRA_` annotation macros for lifetimes, nullability,
  thread safety and unsafe buffer regions.
