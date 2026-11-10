# One `x86-64-v3` release image

Every release publishes one container image, `ghcr.io/sergioffpc/umbra:X.Y.Z`, built with `-C target-cpu=x86-64-v3`, and every Linux Worker pulls it. The per-CPU images `X.Y.Z-znver2` and `X.Y.Z-broadwell` are dropped. Decided with the owner on 2026-11-10. This amends [ADR 0005](0005-chiseled-release-container-image.md), [ADR 0012](0012-per-cpu-binaries-locked-to-one-commit.md), [ADR 0021](0021-plug-and-play-workers-in-a-named-cluster.md) and [ADR 0023](0023-x86-64-only.md).

A per-CPU target buys almost nothing over `x86-64-v3`. The instructions `znver2` and `broadwell` add (ADX, RDSEED, SHA, SSE4a and the like) do no floating-point work, and neither CPU has AVX-512. What is left is LLVM's per-CPU scheduling model, worth a few percent at best and sometimes nothing. Neither benefit was measured, and it cost a CI matrix, one image per CPU and the risk of a Worker pulling the wrong one (a `znver2` binary may use SSE4a, which Intel CPUs lack).

- **One image.** The `Dockerfile` builds for `x86-64-v3` and takes no `TARGET_CPU` build argument. `release-image.yml` runs one job, with one provenance, SBOM and attestation.
- **Tags.** The only tag is `X.Y.Z`. There is still no `X.Y` or `latest`: a moving tag would put the Workers of one Cluster on different commits, which the Build Identity handshake refuses ([ADR 0012](0012-per-cpu-binaries-locked-to-one-commit.md)).
- **Development builds are unchanged.** `cargo build` on each host keeps `target-cpu=native` from `.cargo/config.toml`. The Build Identity still records the target CPU, informational only.
- **Results.** Every Worker of a release runs the same binary. [ADR 0022](0022-determinism-on-one-machine-statistical-equality-across-machines.md) still promises the same bits only on one machine, but one binary in practice gives the same bits on every `x86-64-v3` CPU.

## Considered options

- **Keep the per-CPU images** of ADR 0012: rejected, for the reasons above. Revisit only if a render benchmark on both machines shows a clear gain, a few percent or more, for a per-CPU build.
- **`x86-64-v3` with a per-CPU tuning** (`-Z tune-cpu`): rejected. It keeps the instruction set portable but needs a nightly compiler, and it still means one image per CPU.
- **Restore the `X.Y` and `latest` tags**: rejected. ADR 0012 dropped them so no host could pull another CPU's binary; that reason is gone, but the commit lock still argues against moving tags.

## Consequences

- A machine with a new CPU needs nothing new, as long as it has `x86-64-v3`.
- CI builds the release once, so a release takes one fat-LTO build instead of two.
