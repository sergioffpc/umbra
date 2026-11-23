# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# umbra-deps: the toolchain image plus the third-party libraries built by the
# dependency build (layer "deps"), the sources they were built from and a static
# clangd index of those sources. Everything lives under /opt/umbra:
#
#   include/, lib/, lib/cmake/  the libraries; CMAKE_PREFIX_PATH points here
#   src/                        the dependency build and, in src/<dependency>/,
#                               the (patched) sources, kept for navigation
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
#   /opt/cuda/optix/include    the OptiX headers; OptiX_INSTALL_DIR
#
# The Nsight profilers are developer-only tools, so they stay out of this
# image, which CI pulls too.
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
    curl -fsSLo optix.tar.gz \
      "https://github.com/NVIDIA/optix-dev/archive/refs/tags/v${OPTIX_VERSION}.tar.gz"; \
    echo "$OPTIX_SHA256  optix.tar.gz" | sha256sum -c -; \
    install -d /opt/cuda/optix; \
    for component in cuda_nvcc cuda_crt libnvvm cuda_cudart cccl; do \
      tar -xJf "$component.tar.xz" -C /opt/cuda --strip-components=1; \
    done; \
    tar -xzf optix.tar.gz -C /opt/cuda/optix --strip-components=1; \
    rm -rf /tmp/nvidia
WORKDIR /
# nvcc links from lib64/, as in the toolkit installers; the archives use lib/.
RUN ln -s lib /opt/cuda/lib64 \
 && /opt/cuda/bin/nvcc --version \
 && printf '%s\n' "CUDA ${CUDA_VERSION}, OptiX ${OPTIX_VERSION}" >/opt/cuda/version.txt
ENV PATH=/opt/cuda/bin:${PATH} \
    CUDAToolkit_ROOT=/opt/cuda \
    OptiX_INSTALL_DIR=/opt/cuda/optix

# The dependency build runs from its copy under the prefix, so the compile
# databases and the clangd index point at the sources that stay in the image.
FROM nvidia AS build
COPY third_party/ /opt/umbra/src/
RUN cmake -S /opt/umbra/src -B /tmp/dependency-build -G Ninja \
          -DUMBRA_LAYER=deps \
          -DCMAKE_INSTALL_PREFIX=/opt/umbra \
 && cmake --build /tmp/dependency-build --target layer-deps

FROM nvidia AS deps
COPY --from=build /opt/umbra/ /opt/umbra/
ENV CMAKE_PREFIX_PATH=/opt/umbra

# The devcontainer's non-root user. It takes uid/gid 1000 from Ubuntu's
# default user, which the devcontainer maps the host user onto (podman
# keep-id). The directories mounted as named volumes exist here, owned by dev,
# so that a new volume starts out owned by dev too. CI keeps running as root.
RUN userdel --remove ubuntu \
 && useradd --create-home --uid 1000 --user-group --shell /bin/bash dev \
 && install -d -o dev -g dev /home/dev/.claude /home/dev/.config /home/dev/.config/gh
