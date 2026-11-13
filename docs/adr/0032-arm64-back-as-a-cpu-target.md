# arm64 back as a CPU target

Umbra builds and runs on arm64 again, as a second CPU beside x86-64 with `x86-64-v3`. The release image is published for `linux/amd64` and `linux/arm64`. Decided with the owner on 2026-11-13. This supersedes [ADR 0023](0023-x86-64-only.md) and amends [ADR 0005](0005-chiseled-release-container-image.md), [ADR 0022](0022-determinism-on-one-machine-statistical-equality-across-machines.md) and [ADR 0025](0025-one-x86-64-v3-release-image.md).

ADR 0023 dropped arm64 because it cost a second backend for every SIMD kernel, with NaN and rounding behaviour matched bit for bit across two instruction sets. [ADR 0022](0022-determinism-on-one-machine-statistical-equality-across-machines.md) has since dropped bit-identical results across machines, and [ADR 0028](0028-one-core-for-cpu-and-gpu.md) has added a lane-by-lane backend for `nvptx64` whose contract is the scalar code itself. arm64 reuses that backend, so the cost ADR 0023 weighed is mostly gone. In return the operator can render, and run the release image under Podman or Docker, on an Apple silicon Mac, which cannot run the `x86-64-v3` image: Apple's Virtualization framework emulates x86-64 without AVX2 or FMA.

- **No NEON `Vec4` backend.** `Vec4`/`Mask4` on arm64 use the lane-by-lane backend `simd/lanes.rs` shared with `nvptx64`: each operation is the scalar operation on each lane, and LLVM vectorises the loops to NEON. Hand-written NEON kernels come only if a benchmark asks for them.
- **NEON scalar primitives.** `sqrt`, `fma`, `floor` and `ceil` are NEON intrinsics on lane 0 (`fsqrt`, `fmadd`, `frintm`, `frintp`), behind the same `#[target_feature]` and `unsafe` wrapper as x86-64's ([ADR 0007](0007-unsafe-code-as-an-aggressive-optimisation.md)). NEON is part of every arm64 target, so there is no baseline to require, unlike `x86-64-v3`.
- **Transcendentals.** Umbra's `f64` scalar kernels (`sin_cos`, `atan2`, `erf`) are shared by both CPUs. `Vec4::exp` and `Vec4::pow` are the C library's `expf` and `powf` per lane, within 1 ULP with glibc and with Apple's libm, not the x86-64 backend's own kernels. The C math wrappers serve glibc on Linux and `libSystem` on macOS, which has no `sincosf`. Where the C libraries disagree on a special value, the facade pins it down: a signalling NaN exponent gives `Vec4::pow` NaN.
- **Results.** An arm64 machine gives the same bits run to run, and images statistically equal to x86-64's ([ADR 0022](0022-determinism-on-one-machine-statistical-equality-across-machines.md)).
- **CI.** A second Rust job runs `scripts/check.sh rust` on GitHub's `ubuntu-24.04-arm` runner. Miri and AddressSanitizer stay on x86-64, where the `unsafe` of the `x86_64` backend is.
- **Release image.** One multi-platform image, `X.Y.Z`: `linux/amd64` built for `x86-64-v3` ([ADR 0025](0025-one-x86-64-v3-release-image.md)) and `linux/arm64` for the plain arm64 target. The x86-64 runner builds and smoke-tests the arm64 half under QEMU.

## Considered options

- **Stay x86-64 only** and run the `amd64` image under emulation on Apple silicon: rejected. Podman's `applehv` machine has no Rosetta, and QEMU's x86-64 lacks AVX2 and FMA, so the `x86-64-v3` binary does not start.
- **Restore the NEON backend removed with ADR 0023**: rejected for now. Its point was to match x86-64 bit for bit, which ADR 0022 no longer asks, and it is 420 lines of `unsafe` to maintain. It stays in git history (`crates/umbra-math/src/simd/aarch64.rs` before 4a5cb40).
- **Build the arm64 image on a native arm64 runner** and merge the two into one manifest: deferred. It is faster than QEMU but needs a second job pushing by digest and a third merging them. Worth it if the QEMU build grows too slow for a release.

## Consequences

- An Apple silicon Mac builds and tests the workspace natively, with no `CARGO_BUILD_TARGET` or Rosetta, and runs the arm64 image under Podman.
- Kernels written for AVX2's 8 lanes (arc42 §8.14) need an arm64 path too; the lane-by-lane backend is the default one.
- The arm64 CI runner is on Ubuntu 24.04, an older glibc than the release image's 26.04.
