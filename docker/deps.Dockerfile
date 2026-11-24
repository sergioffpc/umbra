# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# umbra-deps: the toolchain image plus the third-party libraries built by the
# dependency build (layer "deps"), the sources they were built from and a static
# clangd index of those sources. Everything lives under /opt/umbra:
#
#   include/, lib/, lib/cmake/  the libraries; CMAKE_PREFIX_PATH points here
#   tsan/                       oneTBB instrumented with ThreadSanitizer, for
#                               the tsan preset
#   src/                        the dependency build and, in src/<dependency>/,
#                               the (patched) sources, kept for navigation and
#                               for the license texts umbra's install ships
#   share/clangd/deps.idx       clangd index of src/, with absolute paths
#                               under /opt/umbra/src (for an External index)
#
# It also carries the NVIDIA development stack under /opt/cuda (stage
# "nvidia"), which the GPU module and the dependencies with GPU backends build
# against. nvcc, ptxas, libnvvm, the CUDA headers and the OptiX headers are not
# redistributable, so this image is a private package.
#
# Build from the repository root with the layer's submodules checked out, on
# top of the toolchain image whose tag the deps layer hash includes:
#   docker buildx build -f docker/deps.Dockerfile \
#     --build-arg TOOLCHAIN_IMAGE=ghcr.io/<owner>/umbra-toolchain:<tag> .

ARG TOOLCHAIN_IMAGE

# The NVIDIA development stack: prebuilt downloads only, each pinned by version
# and SHA-256, nothing compiled. The CUDA components are NVIDIA's redist
# archives (versions and hashes from redistrib_<CUDA_VERSION>.json); the OptiX
# headers are the release archive of NVIDIA/optix-dev. Layout:
#
#   /opt/cuda                  nvcc, crt, libnvvm, cudart and CCCL (bin/,
#                              include/, lib/, nvvm/); CUDAToolkit_ROOT
#   /opt/cuda/bin              also cuobjdump and nvdisasm, to inspect the
#                              SASS and PTX in a binary, cuda-gdb, to debug
#                              kernels, and compute-sanitizer
#                              (/opt/cuda/compute-sanitizer), the memcheck,
#                              initcheck, racecheck and synccheck tools;
#                              cuda-gdb is GPL-3.0-or-later, and its source
#                              stays in extras/, as NVIDIA ships it
#   /opt/cuda/include          also the cuRAND headers alone, without its
#                              libraries: clang's CUDA runtime wrapper includes
#                              curand_mtgp32_kernel.h, so OSL's shadeops, which
#                              clang compiles to NVPTX, need them
#   /opt/cuda/optix/include    the OptiX headers; OptiX_INSTALL_DIR
#
# The debugger and the sanitizer need a GPU, so only a GPU host uses them. The
# Nsight profilers are developer-only tools, so they stay out of this image,
# which CI pulls too.
#
# Since CUDA 13.4 the toolkit carries no driver: the driver (libcuda.so.1) and
# the OptiX runtime come from the host. nvcc compiles device code only, with
# clang as host compiler (see cmake/UmbraCuda.cmake).
FROM ${TOOLCHAIN_IMAGE} AS nvidia
ARG CUDA_VERSION=13.5.0
ARG CUDA_NVCC_VERSION=13.5.37
ARG CUDA_NVCC_SHA256=ac41262734f3c62cd75a502081cfae093b03b846d7f9274d8b1eb41f83209b96
ARG CUDA_CRT_VERSION=13.5.37
ARG CUDA_CRT_SHA256=0df5c0ca27392152f370751c82b27ff1ab67661e4810cfc7248e2e03479c9ae0
ARG LIBNVVM_VERSION=13.5.37
ARG LIBNVVM_SHA256=8dd4866e387f0ce2259e8dcc91bebdbb59f84c6d4505b3bba46ae91b9a11dd16
ARG CUDA_CUDART_VERSION=13.5.37
ARG CUDA_CUDART_SHA256=26ca1b8963ce1b73e0e77113c711730545cacaaee7233c0526477d96d4c621fe
ARG CUDA_CUOBJDUMP_VERSION=13.5.37
ARG CUDA_CUOBJDUMP_SHA256=cef2d02c05b3f6300ac8972ddb960feb899e614575476bf45dbbdf70c330d2ae
ARG CUDA_NVDISASM_VERSION=13.5.37
ARG CUDA_NVDISASM_SHA256=6af64b980dcd571657350949f0843e7f851b32e7294e1c3f1acc294ad327ee6e
ARG CUDA_SANITIZER_API_VERSION=13.5.25
ARG CUDA_SANITIZER_API_SHA256=483451f82bc238002cd569f8595305d3fc023cd9d36398d033a40edb96b5bcbf
ARG CUDA_GDB_VERSION=13.5.25
ARG CUDA_GDB_SHA256=65d86e2c81dd4d2afcdeef6c839d433aa23f48a4431ccaf5700928853b51855e
ARG LIBCURAND_VERSION=10.4.5.25
ARG LIBCURAND_SHA256=b58397cff0012cf2d143fca148aa7a58831b758cfaf2d4433f35cf267ea9b19b
ARG CCCL_VERSION=13.3.5.0.6
ARG CCCL_SHA256=5139f641e72df6b0c64ee9e68cabaeafa9a5e36a4b1f7d370a1dc0dac62dd72f
ARG OPTIX_VERSION=9.1.0
ARG OPTIX_SHA256=3a29b2254107fdfbb5e6bbad3ec154dd682149121f61e9c406607ac7b52a6ba6
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends curl xz-utils \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /tmp/nvidia
RUN set -eu; \
    redist=https://developer.download.nvidia.com/compute/cuda/redist; \
    fetch() { \
      curl -fsSLo "$1.tar.xz" \
        "$redist/$1/linux-x86_64/$1-linux-x86_64-$2-archive.tar.xz"; \
      echo "$3  $1.tar.xz" | sha256sum -c -; \
    }; \
    fetch cuda_nvcc "$CUDA_NVCC_VERSION" "$CUDA_NVCC_SHA256"; \
    fetch cuda_crt "$CUDA_CRT_VERSION" "$CUDA_CRT_SHA256"; \
    fetch libnvvm "$LIBNVVM_VERSION" "$LIBNVVM_SHA256"; \
    fetch cuda_cudart "$CUDA_CUDART_VERSION" "$CUDA_CUDART_SHA256"; \
    fetch cccl "$CCCL_VERSION" "$CCCL_SHA256"; \
    fetch cuda_cuobjdump "$CUDA_CUOBJDUMP_VERSION" "$CUDA_CUOBJDUMP_SHA256"; \
    fetch cuda_nvdisasm "$CUDA_NVDISASM_VERSION" "$CUDA_NVDISASM_SHA256"; \
    fetch cuda_sanitizer_api "$CUDA_SANITIZER_API_VERSION" \
      "$CUDA_SANITIZER_API_SHA256"; \
    fetch cuda_gdb "$CUDA_GDB_VERSION" "$CUDA_GDB_SHA256"; \
    fetch libcurand "$LIBCURAND_VERSION" "$LIBCURAND_SHA256"; \
    curl -fsSLo optix.tar.gz \
      "https://github.com/NVIDIA/optix-dev/archive/refs/tags/v${OPTIX_VERSION}.tar.gz"; \
    echo "$OPTIX_SHA256  optix.tar.gz" | sha256sum -c -; \
    install -d /opt/cuda/optix; \
    for component in cuda_nvcc cuda_crt libnvvm cuda_cudart cccl \
                     cuda_cuobjdump cuda_nvdisasm cuda_sanitizer_api cuda_gdb; do \
      tar -xJf "$component.tar.xz" -C /opt/cuda --strip-components=1; \
    done; \
    tar -xJf libcurand.tar.xz -C /opt/cuda --strip-components=1 \
      --wildcards '*/include/*'; \
    tar -xzf optix.tar.gz -C /opt/cuda/optix --strip-components=1; \
    rm -rf /tmp/nvidia
WORKDIR /
# nvcc links from lib64/, as in the toolkit installers; the archives use lib/.
RUN ln -s lib /opt/cuda/lib64 \
 && /opt/cuda/bin/nvcc --version \
 && /opt/cuda/bin/cuobjdump --version \
 && /opt/cuda/bin/cuda-gdb --version \
 && /opt/cuda/bin/compute-sanitizer --version \
 && printf '%s\n' "CUDA ${CUDA_VERSION}, OptiX ${OPTIX_VERSION}" >/opt/cuda/version.txt
ENV PATH=/opt/cuda/bin:${PATH} \
    CUDAToolkit_ROOT=/opt/cuda \
    OptiX_INSTALL_DIR=/opt/cuda/optix

# The dependency build runs from its copy under the prefix, so the compile
# databases and the clangd index point at the sources that stay in the image.
# The dependencies of the dependencies are Ubuntu packages, not submodules:
# their -dev packages are installed here, their runtime packages in the final
# stage (see third_party/versions.md).
#
# Every compiler runs through ccache, whose cache is the BuildKit cache mount
# below: the deps workflow loads it before the build and saves it after, so a
# build recompiles only the files whose source, flags or compiler changed.
# The paths of the build are fixed, and ccache hashes the compiler's content,
# so a hit is a file compiled the same way. ccache stays in this stage only.
FROM nvidia AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libhwloc-dev libdeflate-dev libzstd-dev ccache \
      libexpat1-dev zlib1g-dev libminizip-ng-dev \
      libjpeg-turbo8-dev libpng-dev libwebp-dev libgif-dev libopenjp2-7-dev \
      libfreetype-dev flex bison \
 && rm -rf /var/lib/apt/lists/*
ENV CCACHE_DIR=/var/cache/ccache \
    CCACHE_MAXSIZE=5G \
    CCACHE_COMPILERCHECK=content
COPY third_party/ /opt/umbra/src/
RUN --mount=type=cache,id=umbra-deps-ccache,target=/var/cache/ccache,sharing=locked \
    ccache --zero-stats \
 && cmake -S /opt/umbra/src -B /tmp/dependency-build -G Ninja \
          -DUMBRA_LAYER=deps \
          -DCMAKE_INSTALL_PREFIX=/opt/umbra \
 && cmake --build /tmp/dependency-build --target layer-deps \
 && ccache --show-stats

FROM nvidia AS deps
ARG DEBIAN_FRONTEND=noninteractive
# hwloc: oneTBB's libtbbbind, for NUMA-aware task arenas. libdeflate and
# zstd: OpenEXR's ZIP and ZSTD compression. expat, zlib and minizip-ng:
# OpenColorIO's CLF/CTF files and .ocioz archives. libjpeg-turbo, libpng,
# libwebp, giflib, OpenJPEG and FreeType: OpenImageIO's JPEG, PNG, WebP, GIF
# and JPEG 2000 formats and its text rendering; libjpeg, libwebp and zstd
# also libtiff's codecs. zlib and zstd: OpenVDB's grid compression; their
# -dev packages too, since FindOpenVDB.cmake, through which umbra finds
# OpenVDB, finds them for its own targets.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libhwloc15 libdeflate0 libzstd1 libexpat1 zlib1g libminizip-ng4t64 \
      zlib1g-dev libzstd-dev \
      libjpeg-turbo8 libpng16-16t64 libwebp7 libwebpdemux2 libwebpmux3 \
      libgif7 libopenjp2-7 libfreetype6 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/umbra/ /opt/umbra/
ENV CMAKE_PREFIX_PATH=/opt/umbra

# The devcontainer's non-root user. It takes uid/gid 1000 from Ubuntu's
# default user, which the devcontainer maps the host user onto (podman
# keep-id). The directories mounted as named volumes exist here, owned by dev,
# so that a new volume starts out owned by dev too. CI keeps running as root.
RUN userdel --remove ubuntu \
 && useradd --create-home --uid 1000 --user-group --shell /bin/bash dev \
 && install -d -o dev -g dev /home/dev/.claude /home/dev/.config /home/dev/.config/gh
