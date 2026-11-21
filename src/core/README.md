# core

The foundation every other module depends on: what umbra is and how it was
built, how errors are reported, and the per-thread and arithmetic rules all
code follows.

Public headers (`include/umbra/core/`):

- `version.h`: the umbra version, from the CMake project.
- `build_info.h`: the commit (with `-dirty`), build type, target architecture
  and compiler of the binary, generated on every build.
- `error.h`: the `[[nodiscard]]` error type and the `Result<T>` alias of
  `std::expected`.
- `wrapping.h`: wrapping add, sub, mul and shl, the only intentional
  overflow.
- `floating_point.h`: the per-thread FTZ/DAZ configuration, called first on
  every thread.
- `safety.h`: the `UMBRA_` annotation macros for lifetimes, nullability,
  thread safety and unsafe buffer regions.
