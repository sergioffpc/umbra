# Linux only

Umbra builds and runs on Linux with glibc only, on x86-64 and arm64. macOS, Windows and musl are not supported, and `umbra-math` stops the build on any of them. Decided with the owner on 2026-11-13. This amends [ADR 0032](0032-arm64-back-as-a-cpu-target.md), which also built the workspace natively on macOS.

Every target machine runs Linux (§7.1), and the release image is `linux/amd64` and `linux/arm64`. Native macOS was a second C library behind `umbra-math`'s wrappers: `libSystem` has no `sincosf` symbol, and its `powf` returns other values than glibc's at special values. Each difference is a `cfg` and a test to keep, for a platform no Worker runs on.

- **One C library.** `umbra-math`'s wrappers call glibc's `libm.so.6` on both CPUs. The macOS `sincosf` fallback and the `aarch64-apple-darwin` build settings are removed.
- **The build stops elsewhere.** On x86-64 and arm64, a target other than `linux-gnu` is a `compile_error!` in `umbra-math`, as an unsupported architecture already is.
- **arm64 stays.** arm64 Linux is still a CPU target, with the release image's `linux/arm64` build, so an operator's Apple silicon Mac runs Umbra in that image under Podman or Docker.

## Considered options

- **Keep native macOS as best effort**, untested in CI: rejected. A `cfg` branch nothing runs is code that breaks unseen.
- **Drop arm64 too**, back to [ADR 0023](0023-x86-64-only.md): rejected. The Mac would lose its only way to run Umbra, and arm64 Linux costs no second C library.

## Consequences

- An Apple silicon Mac builds and tests the workspace in the dev container or another Linux environment, not natively.
- The glibc `libmvec` tests and benchmarks depend on the architecture alone, x86-64, not on the operating system.
