# Third-party versions

Every git submodule under `third_party/`, pinned to an official release tag.
CI checks that each row matches the submodule commit recorded in the
repository and that each license is on the allow-list. Update the row in the
same commit that moves a submodule.

Columns: `Path` is the submodule path, `Tag` the upstream release tag,
`Commit` the full commit the tag points to, and `License` an SPDX license
expression.

`LicenseRef-TOST-1.0` is the Tomorrow Open Source Technology License 1.0 of
Pixar's projects, which has no SPDX identifier: the Apache License 2.0 with a
different section 6 (Trademarks). `LicenseRef-Ptex` is the license of Disney's
Ptex, which has no SPDX identifier: the BSD 3-Clause License with a
disclaimer that also disclaims the warranties of non-infringement and title.

| Name | Path | Tag | Commit | License |
|---|---|---|---|---|
| LLVM | `third_party/llvm-project` | `llvmorg-23.1.6` | `5b10d2d74929eae78912771222c9855af97496f4` | `Apache-2.0 WITH LLVM-exception` |
| GoogleTest | `third_party/googletest` | `v1.18.0` | `063de7e9578f82b369302001269680b4b1553359` | `BSD-3-Clause` |
| Google Benchmark | `third_party/benchmark` | `v1.10.0` | `a6b4a9f00b36c19af9997d68a1cf6e0b5b3d032c` | `Apache-2.0` |
| oneTBB | `third_party/oneTBB` | `v2023.2.0` | `41158a44ab3fb7ac5df7f99ef82626c87607447f` | `Apache-2.0` |
| Embree | `third_party/embree` | `v4.5.0` | `3b7e4ee746ce8ed25b206fb2f4d2709e9f8109ce` | `Apache-2.0` |
| OpenQMC | `third_party/openqmc` | `v0.7.1` | `72617f52751d53a4ca49f60fd0cb4f5946236f2f` | `Apache-2.0` |
| Imath | `third_party/Imath` | `v3.2.3` | `5f27ba266d3ea1565e912570c30b5eafc89959f1` | `BSD-3-Clause` |
| OpenEXR | `third_party/openexr` | `v3.5.5` | `486e238dd28ba6fed22528005b05f9f15bef0cc0` | `BSD-3-Clause` |
| OpenSubdiv | `third_party/OpenSubdiv` | `v3_8_0` | `1d90921ea32b9dcae31d30b907cbacefc0c616bf` | `LicenseRef-TOST-1.0` |
| MaterialX | `third_party/MaterialX` | `v1.39.5` | `7b64921ef1d42f2d57871e9d2c43dc11f041f26b` | `Apache-2.0` |
| yaml-cpp | `third_party/yaml-cpp` | `yaml-cpp-0.9.0` | `56e3bb550c91fd7005566f19c079cb7a503223cf` | `MIT` |
| pystring | `third_party/pystring` | `v1.2.0` | `1922c8f2b48e3beb6831c27b4811b58995e986cf` | `BSD-3-Clause` |
| OpenColorIO | `third_party/OpenColorIO` | `v2.6.0` | `10a43ec1df03d072ca9a9e2fb3f26e454b0900d8` | `BSD-3-Clause` |
| {fmt} | `third_party/fmt` | `12.2.1` | `208219daff58f2a7438f6931cc2ddc358acf33ef` | `MIT` |
| robin-map | `third_party/robin-map` | `v1.4.1` | `bd14e6830a1474fed9d2d03f5c3b0683d818d540` | `MIT` |
| libtiff | `third_party/libtiff` | `v4.7.2` | `d01a94be176f5f6a87f7ee1c0b32e65416aa2b4d` | `libtiff` |
| pugixml | `third_party/pugixml` | `v1.16` | `c8033ce9d039e7f9d134877c363397b3cfe20816` | `MIT` |
| Ptex | `third_party/ptex` | `v2.5.4` | `42787491dbea182e339074d305b782b14f1ca2be` | `LicenseRef-Ptex` |
| OpenImageIO | `third_party/OpenImageIO` | `v3.2.2.0` | `96377f681cc9e6650c8534671f4fc36b3c8acbc1` | `Apache-2.0` |
| OpenVDB | `third_party/openvdb` | `v14.0.0` | `ecae2bda85744412b0cd47e1b8e1e4e76beb7b88` | `Apache-2.0` |
| OSL | `third_party/OpenShadingLanguage` | `v1.15.8.0` | `b64c29cc2540c398265942edc44650aea3a8c103` | `BSD-3-Clause` |
| OpenUSD | `third_party/OpenUSD` | `v26.11.1` | `96c1989167d2aaab5bd34627f32a321a6b79866b` | `LicenseRef-TOST-1.0` |
| Open Image Denoise | `third_party/oidn` | `v2.5.1` | `6602ee2ca38a1a2a02135beed8f6e68eed630180` | `Apache-2.0` |

Open Image Denoise has nested submodules of its own, pinned by its commit:
`weights`, the trained weights of its filters, whose files are in Git LFS
(`Apache-2.0`), and `external/cutlass`, NVIDIA's CUTLASS, which its CUDA
device compiles in (`BSD-3-Clause`). `forge checkout-submodules` checks out
these two and pulls the weights; it leaves out `external/composable_kernel`,
which only the HIP device uses.

## Ubuntu packages

The C dependencies of the submodules are not submodules: they are Ubuntu
packages of the image's release, installed by `docker/deps.Dockerfile`, the
`-dev` package in the stage that builds the submodules and the runtime package
in the image. zlib and zstd keep their `-dev` packages in the image too: umbra
finds OpenVDB through `FindOpenVDB.cmake`, which finds them. Their C++ dependencies are submodules, built against libc++,
because Ubuntu builds its C++ libraries against libstdc++. So is libtiff, a
C library: Ubuntu's `libtiff6` links the C++ `liblerc4`.

The release image, built by `docker/release.Dockerfile`, takes the runtime
packages that umbra loads, and those they depend on, as Chisel slices cut
from the same release: those of `canonical/chisel-releases`, at the commit
the Dockerfile pins, and, for a package it does not slice yet, ours in
`docker/chisel/slices/`.

| Name | Needed by | Packages | License |
|---|---|---|---|
| hwloc | oneTBB (`libtbbbind_2_5`, NUMA-aware task arenas) | `libhwloc-dev`, `libhwloc15` | `BSD-3-Clause` |
| libdeflate | OpenEXR (ZIP compression), Ptex (compression) | `libdeflate-dev`, `libdeflate0` | `MIT` |
| zstd | OpenEXR, OpenVDB (ZSTD compression) | `libzstd-dev`, `libzstd1` | `BSD-3-Clause OR GPL-2.0-only` |
| expat | OpenColorIO (CLF and CTF transform files) | `libexpat1-dev`, `libexpat1` | `MIT` |
| zlib | OpenColorIO, through minizip-ng; OpenVDB and NanoVDB (ZIP compression) | `zlib1g-dev`, `zlib1g` | `Zlib` |
| minizip-ng | OpenColorIO (`.ocioz` config archives) | `libminizip-ng-dev`, `libminizip-ng4t64` | `Zlib` |
| libjpeg-turbo | OpenImageIO (JPEG), libtiff (JPEG compression) | `libjpeg-turbo8-dev`, `libjpeg-turbo8` | `IJG AND BSD-3-Clause AND Zlib` |
| libpng | OpenImageIO (PNG), FreeType | `libpng-dev`, `libpng16-16t64` | `libpng-2.0` |
| libwebp | OpenImageIO (WebP), libtiff (WebP compression) | `libwebp-dev`, `libwebp7`, `libwebpdemux2`, `libwebpmux3` | `BSD-3-Clause` |
| giflib | OpenImageIO (GIF) | `libgif-dev`, `libgif7` | `MIT` |
| OpenJPEG | OpenImageIO (JPEG 2000) | `libopenjp2-7-dev`, `libopenjp2-7` | `BSD-2-Clause` |
| FreeType | OpenImageIO (text rendering) | `libfreetype-dev`, `libfreetype6` | `FTL OR GPL-2.0-or-later` |
| flex | OSL (the lexers of oslc and the .oso reader), build only | `flex` | `BSD-3-Clause` |
| GNU Bison | OSL (their parsers), build only | `bison` | `GPL-3.0-or-later WITH Bison-exception-2.2` |

## Open-source build-time downloads

Prebuilt tools that build a submodule and stay out of what umbra ships,
downloaded by the `ispc` stage of `docker/toolchain.Dockerfile`, which pins
the archive by version and SHA-256.

| Name | Version | Source | License |
|---|---|---|---|
| ISPC | 1.32.0 | `ispc/ispc` release archive | `BSD-3-Clause` |

## Proprietary build-time downloads

Not submodules, and so not on the license allow-list: the NVIDIA development
stack of the GPU module, downloaded by the `nvidia` stage of
`docker/toolchain.Dockerfile`, which pins each archive by version and SHA-256.
They are not redistributable, which is why the `umbra-toolchain` image, and
the `umbra-deps` image built on it, are private packages. Only the CUDA runtime ends up in what umbra ships: `libcudart_static.a`,
linked into `libumbra-gpu.so` and into OpenSubdiv's osdGPU, is redistributable
inside an application under the CUDA EULA (Attachment A). The shared
`libcudart.so` is never used: it needs libstdc++ and libgcc_s.

| Name | Version | Source | License |
|---|---|---|---|
| CUDA Toolkit components (nvcc, crt, libnvvm, cudart) | 13.5.37 (CUDA 13.5.0) | NVIDIA CUDA redist archives | CUDA Toolkit EULA |
| CUDA developer tools (cuobjdump, nvdisasm) | 13.5.37 (CUDA 13.5.0) | NVIDIA CUDA redist archives | CUDA Toolkit EULA |
| CUDA developer tools (compute-sanitizer, cuda-gdb) | 13.5.25 (CUDA 13.5.0) | NVIDIA CUDA redist archives | CUDA Toolkit EULA (cuda-gdb: GPL-3.0-or-later, with its source in `/opt/cuda/extras/`) |
| CCCL | 13.3.5.0.6 | NVIDIA CUDA redist archives | CCCL EULA |
| cuRAND headers (no libraries) | 10.4.5.25 (CUDA 13.5.0) | NVIDIA CUDA redist archives | CUDA Toolkit EULA |
| OptiX headers | 9.1.0 | `NVIDIA/optix-dev` release archive | NVIDIA DesignWorks SDK license (BSD-3-Clause for some headers) |
