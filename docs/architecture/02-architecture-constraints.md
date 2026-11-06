# 2. Architecture Constraints

Constraints are what the architecture must accept rather than choose. Some come from outside the project, such as the machines and the reference renderer. Others are standing decisions of the owner, made when the [Umbra implementation plan](https://github.com/sergioffpc/umbra/issues/9) was charted and not revisited by later decisions.

## 2.1 Technical constraints

| Constraint | Background |
| --- | --- |
| **Rust 2024, stable, pinned toolchain** | `rust-toolchain.toml` pins the compiler and `rust-version` follows it. No nightly features, so no `std::simd` ([SIMD options](https://github.com/sergioffpc/umbra/issues/16)). |
| **CPU only** | No GPU backend, although the P620 has a GPU. The CPU code still respects the GPU-readiness rules of §8.12 ([ADR 0014](../adr/0014-gpu-ready-no-std-rendering-core.md)). |
| **Three target machines** | MacBook Pro M2 (arm64, macOS), Lenovo P620 (Zen 2, Linux) and Dell R630 (2 × Broadwell-EP, Linux), inventoried in §7.1. No target has AVX-512, so x86 kernels stop at AVX2 + FMA. |
| **One LAN at 1 GbE** | Coordinator and Workers share one LAN, designed for 1 GbE; the MacBook joins over Wi-Fi (§7.1). |
| **The Scene fits in each Worker's memory** | The Scene is replicated to every Worker. There is no out-of-core rendering. |
| **pbrt-v4 as the specification** | pbrt-v4's `.pbrt` format, unextended, is the only input ([ADR 0003](../adr/0003-pbrt-v4-scene-files-parsed-on-the-coordinator.md)). pbrt-v4 and `mmp/pbrt-v4-scenes`, at pinned commits, are the reference for correctness and speed (§8.11). |
| **Safe Rust** | `unsafe_code = "deny"`; an exception is an aggressive optimisation ([ADR 0007](../adr/0007-unsafe-code-as-an-aggressive-optimisation.md)). |
| **Lint thresholds** | The `CONTRIBUTING.md` section "Lint and formatting rules" fixes function length, complexity, nesting and argument count, enforced on every commit (QS-3). |

## 2.2 Organisational constraints

| Constraint | Background |
| --- | --- |
| **One owner** | The owner decides scope and runs every measurement on the target machines by hand. No self-hosted runners and no GitHub-triggered execution on those machines (§8.4). |
| **GitHub** | Issues, milestones, CI, releases and container images live on GitHub (`sergioffpc/umbra`, `ghcr.io`). |
| **Git flow** | `main` holds tagged releases, `develop` integrates, feature branches merge into `develop` through PRs (`CONTRIBUTING.md`). |
| **Conventional Commits** | Every commit message and PR title (`CONTRIBUTING.md`). |
| **MIT licence** | Every dependency must be compatible with it; `cargo-deny` checks licences and advisories. |
| **English artifacts** | Code, ADRs, this document, `GLOSSARY.md` and issues are in English. |
| **Out of scope by decision** | GPU implementation, cloud deployment, autoscaling, out-of-core Scenes and hostile-node security (§3.3). |

## 2.3 Conventions

| Convention | Background |
| --- | --- |
| **Readability first** | An aggressive optimisation is allowed only with a benchmark showing the end-to-end gain, isolation behind a simple contract, and a readable, book-faithful reference implementation it is tested against (§1.2, §8.4). |
| **Book traceability** | Every rendering-core item that implements a pbrt-v4 algorithm cites its book section and pbrt-v4 symbol (QS-4). |
| **Domain language** | The terms of the root [`GLOSSARY.md`](../../GLOSSARY.md) are used in code, documents and issues; pbrt-v4 terms keep the book's meaning. |
| **Choices as enums** | Every choice is an enum with named variants, never a `bool` flag (`CLAUDE.md`). |
| **Architecture records** | Hard-to-reverse decisions are ADRs in `docs/adr/` (section 9); this document describes the result. |
