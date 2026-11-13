# A pinned nightly toolchain

> **Amended by [ADR 0033](0033-one-crate-per-render-device-over-a-shared-contract.md).** `umbra-kernels` is renamed `umbra-gpu-programs`, which enables its unstable features on `nvptx64` only. The rest of this ADR still applies.

The whole workspace builds with one pinned nightly toolchain, `nightly-YYYY-MM-DD` in `rust-toolchain.toml`, instead of stable `1.99.1`. Decided with the owner on 2026-11-11. This amends [ADR 0023](0023-x86-64-only.md), which rejected `-Zbuild-std` because it needs nightly.

The GPU kernels of [ADR 0026](0026-gpu-workers-from-the-first-milestone.md) need unstable features: `abi_ptx` for `extern "ptx-kernel"` entry points, `stdarch_nvptx` for thread and block indices, and `asm_experimental_arch` for the inline PTX that OptiX's device API requires. The owner preferred one toolchain for every crate over a second toolchain for the kernels alone.

- **Pinned, not floating.** The channel names a date, and `clippy`, `rustfmt` and `llvm-tools` must be available for that date. It is bumped deliberately, in its own commit, with the new lints fixed, as stable was before.
- **Unstable features where they are needed.** Only `umbra-kernels` enables `#![feature(...)]`. Every other crate stays on features that are stable, so a crate does not depend on nightly unless it has to.
- **No minimum supported Rust version.** `rust-version` leaves `Cargo.toml`: a nightly has no version to state.
- **Miri.** The Miri job already used a nightly; it now uses the pinned one.

## Considered options

- **A pinned nightly for the kernels crate only**, outside the workspace with its own `rust-toolchain.toml`: rejected by the owner. Two toolchains, with two LLVMs, would compile the core.
- **Stable `1.99.1` with `RUSTC_BOOTSTRAP=1` for the kernels build only**: rejected by the owner. It keeps one compiler but relies on a mechanism the Rust project reserves for its own bootstrap.

## Consequences

- A nightly can carry codegen or lint regressions that a stable release would not. The pin limits this to the bumps, and CI catches them.
- `-Zbuild-std` and other `-Z` flags become available. Using one is still a decision of its own.
