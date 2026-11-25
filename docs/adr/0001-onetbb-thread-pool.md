# oneTBB is the thread pool of the process

All parallel work in the umbra process runs on one oneTBB thread pool: every
umbra target links `TBB::tbb`, and the dependencies that can run on oneTBB
are built to do so (Embree's tasking system is TBB), so that no library
oversubscribes the CPU with a pool of its own. oneTBB supports building
without exceptions and RTTI, so modules include its headers directly, without
a bridge in `src/ext/`. libtbb is built with NUMA support (libtbbbind on
hwloc), since umbra runs on NUMA machines.

## Consequences

- libtbb reports out-of-memory and API misuse (a non-positive step, a
  `task_group` destroyed without `wait()`) by throwing; through umbra's code,
  built without exceptions, that ends in `std::terminate`, as an allocation
  failure in libc++ already does.
- ThreadSanitizer cannot see libtbb's synchronization unless libtbb is
  instrumented too, so the tsan preset links a ThreadSanitizer build of it
  (see `cmake/UmbraSanitizers.cmake`).
