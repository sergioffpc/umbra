# Per-CPU binaries, locked to one commit per Render Job

Each target machine runs a binary compiled for its own CPU, and the Coordinator accepts only Workers built from its own commit. Decided in [Per-machine build and deployment](https://github.com/sergioffpc/umbra/issues/27). This amends [ADR 0005](0005-chiseled-release-container-image.md), which published a single `x86-64-v3` image.

- **One target CPU per machine.** The P620 runs `target-cpu=znver2`, the R630 runs `broadwell` and the MacBook runs `apple-m2` (inventory in [arc42 §7.1](../architecture/07-deployment-view.md)). There is no runtime multiversioning and no generic `x86-64-v3` binary. Adding a machine with a new CPU means adding one target.
- **Releases.** On a `vX.Y.Z` tag, CI publishes one image variant per Linux CPU, `ghcr.io/sergioffpc/umbra:X.Y.Z-znver2` and `:X.Y.Z-broadwell`. Each variant has its own provenance and SBOM. The `Dockerfile` takes a `TARGET_CPU` build argument. The `X.Y`, `latest` and untagged-CPU tags are dropped, so no host can pull a binary built for another CPU. The Mac release is a native `cargo build --release` from the tag.
- **Development builds.** `cargo build` runs directly on each host, with the toolchain pinned by `rust-toolchain.toml` and `target-cpu=native` from `.cargo/config.toml`. On these machines that produces the same CPUs as the release targets. Development builds are not containerised and not cross-compiled.
- **Build Identity handshake.** Every binary embeds a Build Identity: the git commit, whether the tree was dirty, and its target CPU. A container build receives the commit as a build argument, because `.git` is not in the build context. When a Worker connects, the Coordinator rejects it if its commit differs from the Coordinator's own. Dirty builds are rejected unless the Render Job is started with `--allow-dirty`, which is recorded in the Render Job report. The target CPU is informational only. The handshake's exact format belongs to the wire protocol.

Different target CPUs do not threaten [bit-identical results](0002-bit-identical-results-across-architectures.md). FMA is explicit, Rust never contracts floating-point expressions, and SIMD paths must match scalar ones, so `target-cpu` changes instruction selection and scheduling but never results. The cross-architecture determinism tests remain the safety net.

## Considered options

- **One portable `x86-64-v3` binary**: rejected in favour of tuning each machine's instruction scheduling. The single-artifact simplicity it offered is kept for releases by a CI matrix.
- **Runtime multiversioning** (`is_x86_feature_detected!`, `multiversion`): rejected. It only pays off on heterogeneous hardware, and no target has AVX-512.
- **Building releases on each host**: rejected. It would need a toolchain on every Worker and would lose the CI attestations.
- **Cross-compiling from the Mac** (`cargo-zigbuild`): rejected. Releases would depend on a laptop, and a second build path is unnecessary.
- **Per-commit CI images for development**: rejected. With fat LTO and `codegen-units = 1`, every `develop` merge would cost two slow builds, and feature branches would still be uncovered.
- **Protocol-version or semver compatibility checks**: rejected. Workers on different commits would silently break the [deterministic Film](0004-deterministic-film-for-a-fixed-render-job.md), and the crate version does not change between development commits.

## Consequences

- Every Linux host needs Docker for releases and rustup for development. The toolchain installation is a one-time step per host, documented in arc42 §7.
- The tie between a host and its target CPU lives in the operator's host list, used by the launch script, and is not in the binary.
