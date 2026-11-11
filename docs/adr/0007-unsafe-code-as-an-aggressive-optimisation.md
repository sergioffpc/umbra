# `unsafe` code as an aggressive optimisation

> **Amended by [ADR 0027](0027-a-pinned-nightly-toolchain.md).** Miri runs on the nightly the whole workspace pins, not on one pinned for its job only.
>
> **Amended on 2026-11-11 by [#142](https://github.com/sergioffpc/umbra/issues/142).** FFI to the C math library (glibc on x86-64, `libdevice` on `nvptx64`) is an `unsafe` exception of its own, without a benchmark or a safe reference. On x86-64 it is verified by Miri with `-Zmiri-native-lib`, native tests and AddressSanitizer; on `nvptx64`, by the GPU test script.

Umbra code is safe Rust by default. `unsafe_code = "deny"` applies to every crate of the workspace. SIMD, byte casts, thread pinning and allocators come from crates with safe APIs (`wide` behind the `Vec4`/`Mask4` facade, `bytemuck`, …), so the expected amount of `unsafe` in Umbra itself is zero.

An `unsafe` exception is treated as an **aggressive optimisation** and has to meet the same rule as any other: a benchmark showing the gain end-to-end ([arc42 §8.4](../architecture/08-crosscutting-concepts.md#84-performance-measurement)), isolation behind a simple safe contract, and a readable, book-faithful safe reference implementation that the unsafe version is tested against. On top of that rule:

- **Narrowest scope.** The exception is a `#[expect(unsafe_code, reason = "...")]` on the narrowest item possible, never on a module or crate. The lint stays at `deny` and never at `forbid`, because `forbid` cannot be overridden locally.
- **Documented.** Every `unsafe` block has a `// SAFETY:` comment stating why its preconditions hold, and holds only one unsafe operation. Every `unsafe fn` has a `# Safety` section. `SAFETY` comments appear only on `unsafe` code. All of this is enforced by lints.
- **Verified by tools.** The first exception brings a CI job that runs the tests of every `unsafe` item under Miri, on a nightly toolchain pinned for that job only. Where Miri cannot execute the code (unsupported `std::arch` intrinsics, FFI other than the C math below), those tests run under AddressSanitizer on Linux nightly. The tests include the equivalence tests against the safe reference. ThreadSanitizer is not required, because parallelism comes from crates with safe APIs.

**FFI to the C math library** is an exception of another kind. `umbra-math` declares the glibc functions it calls on x86-64, and the `libdevice` functions it calls on `nvptx64`, in `extern "C"` blocks ([ADR 0022](0022-determinism-on-one-machine-statistical-equality-across-machines.md), [ADR 0028](0028-one-core-for-cpu-and-gpu.md)). It replaces the `libm` crate rather than optimising Umbra's code, so it needs no benchmark and has no safe reference beside it. The other rules hold:

- **A safe wrapper per function.** Each C function has exactly one safe wrapper in `umbra-math`, and nothing else calls the declaration. The wrapper holds one call per `unsafe` block, with a `// SAFETY:` comment on each.
- **One exception: `umbra-spectrum`'s build script.** It takes `cbrt` for the RGB-to-spectrum table from the same wrappers, but compiles `umbra-math/src/platform/x86_64.rs` into itself by `#[path]` rather than depending on `umbra-math`. A build-dependency would hit the crate's x86-64-v3 `compile_error!`: with `--target` (the AddressSanitizer run), Cargo passes no RUSTFLAGS to build scripts, so they build for the bare x86-64 baseline. The file depends on `core` only and is the same source, declarations, wrappers and `SAFETY` comments alike, so the second copy adds no `unsafe` of its own and the tests of `umbra-math` cover it.
- **Verified by Miri and by native tests.** On x86-64, the tests that call glibc run under Miri with `-Zmiri-native-lib=/usr/lib/x86_64-linux-gnu/libm.so.6`. Miri checks the Rust side of each call, the pointers that `sincosf` writes through included, but it trusts the `extern` declaration. Native tests compare each wrapper with known values, which catches a wrong signature.
- **AddressSanitizer** covers what Miri cannot see inside glibc: those tests also run under AddressSanitizer on the pinned nightly.
- **On `nvptx64`**, where neither Miri nor AddressSanitizer runs, the GPU test script checks each `libdevice` wrapper against glibc within its documented bound ([ADR 0026](0026-gpu-workers-from-the-first-milestone.md)).

There is no fixed list of modules allowed to contain `unsafe`. Each exception is justified on its own, wherever it lives.

## Considered options

- **`forbid` everywhere**, with all `unsafe` delegated to external crates: rejected. It rules out optimisations a benchmark may later justify, such as unchecked indexing into the arenas or hand-written `std::arch` kernels where `wide` falls short.
- **A fixed set of unsafe modules** (`simd`, `arena`, `platform`), with `unsafe` free inside them: rejected. It fixes module boundaries before they are designed, and makes `unsafe` acceptable by location rather than by measured need.
- **A Miri CI job from day one**: deferred. With no `unsafe` code it would test nothing, so it lands with the first exception.

## Consequences

- An `unsafe` PR carries more weight than any other optimisation: benchmark report, safe reference, equivalence tests, Miri or ASan job. A C math wrapper needs no benchmark or safe reference, but the same verification.
- Reviewers can find every exception with a search for `expect(unsafe_code`.

## Status

The first exception is the SIMD backend of `umbra-math` (`Vec4`/`Mask4` over `core::arch`, which replaced `wide`) and its scalar `fma`/`sqrt`/`floor`/`ceil` primitives: `unsafe` is confined to one intrinsic per block in the `x86_64` and `aarch64` backends, the safe references are the scalar code in `tests/umbra_math/reference/` and, for `Vec4`, the scalar operation per lane (`tests/umbra_math/simd_test.rs`), `tests/umbra_math/equivalence/` holds the bit-for-bit equivalence tests, `benches/kernels.rs` the benchmark, and the `miri` CI job runs the crate's tests under Miri with the `x86-64-v3` baseline the crate requires. See the crate's `README.md`.
