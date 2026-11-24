# Third-party versions

Every git submodule under `third_party/`, pinned to an official release tag.
CI checks that each row matches the submodule commit recorded in the
repository and that each license is on the allow-list. Update the row in the
same commit that moves a submodule.

Columns: `Path` is the submodule path, `Tag` the upstream release tag,
`Commit` the full commit the tag points to, and `License` an SPDX license
expression.

| Name | Path | Tag | Commit | License |
|---|---|---|---|---|
| LLVM | `third_party/llvm-project` | `llvmorg-23.1.6` | `5b10d2d74929eae78912771222c9855af97496f4` | `Apache-2.0 WITH LLVM-exception` |
| GoogleTest | `third_party/googletest` | `v1.18.0` | `063de7e9578f82b369302001269680b4b1553359` | `BSD-3-Clause` |
| Google Benchmark | `third_party/benchmark` | `v1.10.0` | `a6b4a9f00b36c19af9997d68a1cf6e0b5b3d032c` | `Apache-2.0` |
| oneTBB | `third_party/oneTBB` | `v2023.2.0` | `41158a44ab3fb7ac5df7f99ef82626c87607447f` | `Apache-2.0` |
| Embree | `third_party/embree` | `v4.5.0` | `3b7e4ee746ce8ed25b206fb2f4d2709e9f8109ce` | `Apache-2.0` |
| OpenQMC | `third_party/openqmc` | `v0.7.1` | `72617f52751d53a4ca49f60fd0cb4f5946236f2f` | `Apache-2.0` |
| Imath | `third_party/Imath` | `v3.2.3` | `5f27ba266d3ea1565e912570c30b5eafc89959f1` | `BSD-3-Clause` |

## Ubuntu packages

The dependencies of the submodules are not submodules: they are Ubuntu
packages of the image's release, installed by `docker/deps.Dockerfile`, the
`-dev` package in the stage that builds the submodules and the runtime package
in the image. Only the libraries umbra itself builds against are submodules.

| Name | Needed by | Packages | License |
|---|---|---|---|
| hwloc | oneTBB (`libtbbbind_2_5`, NUMA-aware task arenas) | `libhwloc-dev`, `libhwloc15` | `BSD-3-Clause` |

## Proprietary build-time downloads

Not submodules, and so not on the license allow-list: the NVIDIA development
stack of the GPU module, downloaded by the `nvidia` stage of
`docker/deps.Dockerfile`, which pins each archive by version and SHA-256.
They are not redistributable, which is why the `umbra-deps` image is a private
package. Only the CUDA runtime ends up in what umbra ships: `libcudart_static.a`,
linked into `libumbra-gpu.so`, is redistributable inside an application under
the CUDA EULA (Attachment A).

| Name | Version | Source | License |
|---|---|---|---|
| CUDA Toolkit components (nvcc, crt, libnvvm, cudart) | 13.5.37 (CUDA 13.5.0) | NVIDIA CUDA redist archives | CUDA Toolkit EULA |
| CCCL | 13.3.5.0.6 | NVIDIA CUDA redist archives | CCCL EULA |
| OptiX headers | 9.1.0 | `NVIDIA/optix-dev` release archive | NVIDIA DesignWorks SDK license (BSD-3-Clause for some headers) |
