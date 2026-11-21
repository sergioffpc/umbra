# 2. Architecture constraints

## Technical

| Id | Constraint |
|----|------------|
| T1 | C++23, built with the LLVM toolchain only (clang, libc++, compiler-rt, libunwind, lld). No GCC runtime is loaded. CUDA code is compiled by clang. |
| T2 | Linux on x86-64. The architecture may require nothing beyond `x86-64-v3` (AVX2, FMA); higher ISA levels are optional paths only. |
| T3 | GPU executors are NVIDIA only, through CUDA and OptiX, and optional at build time. All executors have full feature parity ([ADR 0002](../adr/0002-cuda-optix-gpu-executors.md)). |
| T4 | USD through Hydra 2.0 is the only scene entry point. There is no proprietary scene format. |
| T5 | Shading uses MaterialX (OpenPBR Surface as reference) compiled to OSL. Umbra implements every closure spectrally. Spectral IOR and fluorescence are authored through Umbra extensions to MaterialX and USD. |
| T6 | ASWF libraries are used wherever they apply: OIIO, OCIO, OpenEXR and Imath, OpenVDB. OpenQMC and OpenPGL are candidates. |
| T7 | Embree is the CPU traversal backend and OptiX the GPU one. Each accelerates only the shards local to its executor. |
| T8 | oneTBB is the CPU task model, enabled in every dependency that supports it. |
| T9 | Every external interface is RGB. The spectral domain lives entirely inside Umbra. |
| T10 | Communication between executors and render nodes is transport-agnostic. It works over commodity Ethernet and does not preclude RDMA. |

## Organisational

| Id | Constraint |
|----|------------|
| O1 | Open source under Apache-2.0. Every mandatory dependency is permissively licensed; proprietary ones (CUDA, OptiX) are always optional. |
| O2 | Pre-1.0: any minor release may break the API. |
| O3 | The devcontainer is the only supported development environment. |
| O4 | Work is tracked in GitHub Issues and follows git flow. |

## Conventions

| Id | Constraint |
|----|------------|
| C1 | Architecture is documented in arc42, sections 1 to 8. Decisions go in [`docs/adr/`](../adr/) and vocabulary in [`GLOSSARY.md`](../../GLOSSARY.md). |
| C2 | Building-block contracts use the message-oriented notation: messages plus a guarded protocol state machine. |
| C3 | Public artifacts are written in English. |
