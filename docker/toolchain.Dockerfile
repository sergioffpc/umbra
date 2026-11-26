# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# umbra-toolchain: Ubuntu 26.04 with LLVM 23 built from the llvm-project
# submodule by the dependency build (layer "toolchain"), ISPC and the NVIDIA
# development stack, plus the C-ABI libraries and build tools that come from
# Ubuntu, git, git-lfs and ssh-keygen (for SSH commit signing), release tools
# (git-cliff, git-flow-next), the lint tools (actionlint, shellcheck and the
# Python lint and format tools) and Linux perf.
#
# nvcc, ptxas, libnvvm, the CUDA headers and the OptiX headers are not
# redistributable, so this image, and the deps image built on it, are
# private packages.
#
# Build from the repository root with the submodules checked out:
#   docker buildx build -f docker/toolchain.Dockerfile .
#
# The LLVM build tree lives in the cache mount "umbra-toolchain-build" so that
# an interrupted build resumes; the deps workflow persists it between runs.

ARG UBUNTU=ubuntu:26.04

# Release binaries of git-cliff, git-flow-next, actionlint and shellcheck,
# verified by SHA256. All four are statically linked and need no GCC runtime
# (for git-cliff that is the musl build). actionlint runs shellcheck on the
# workflows' scripts when it is on PATH.
FROM ${UBUNTU} AS build
ARG DEBIAN_FRONTEND=noninteractive
ARG GIT_CLIFF_VERSION=2.14.2
ARG GIT_CLIFF_SHA256=fa21e2fc70046fcfd0e43e5e4613873ab799aff3d9dc8559c00d31b270e417d0
ARG GIT_FLOW_NEXT_VERSION=2.1.0
ARG GIT_FLOW_NEXT_SHA256=e335b5818f2b2f820c17844c194030d0db78f3cdfa9bb999fcb9ce092ba9ecd2
ARG ACTIONLINT_VERSION=1.7.12
ARG ACTIONLINT_SHA256=8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8
ARG SHELLCHECK_VERSION=0.11.0
ARG SHELLCHECK_SHA256=8c3be12b05d5c177a04c29e3c78ce89ac86f1595681cab149b65b97c4e227198
SHELL ["/bin/bash", "-o", "errexit", "-o", "nounset", "-o", "pipefail", "-c"]
RUN apt-get update \
  && apt-get install -y --no-install-recommends ca-certificates curl xz-utils \
  && rm -rf /var/lib/apt/lists/*
WORKDIR /tmp/tools
# fetch FILE URL SHA256 downloads URL to FILE and verifies it.
RUN readonly GITHUB=https://github.com; \
  fetch() { \
    local file="$1" url="$2" sha256="$3"; \
    curl -fsSLo "${file}" "${url}"; \
    echo "${sha256}  ${file}" | sha256sum -c -; \
  }; \
  fetch git-cliff.tar.gz \
    "${GITHUB}/orhun/git-cliff/releases/download/v${GIT_CLIFF_VERSION}/\
git-cliff-${GIT_CLIFF_VERSION}-x86_64-unknown-linux-musl.tar.gz" \
    "${GIT_CLIFF_SHA256}"; \
  fetch git-flow-next.tar.gz \
    "${GITHUB}/gittower/git-flow-next/releases/download/\
v${GIT_FLOW_NEXT_VERSION}/\
git-flow-next-v${GIT_FLOW_NEXT_VERSION}-linux-amd64.tar.gz" \
    "${GIT_FLOW_NEXT_SHA256}"; \
  fetch actionlint.tar.gz \
    "${GITHUB}/rhysd/actionlint/releases/download/v${ACTIONLINT_VERSION}/\
actionlint_${ACTIONLINT_VERSION}_linux_amd64.tar.gz" \
    "${ACTIONLINT_SHA256}"; \
  fetch shellcheck.tar.xz \
    "${GITHUB}/koalaman/shellcheck/releases/download/v${SHELLCHECK_VERSION}/\
shellcheck-v${SHELLCHECK_VERSION}.linux.x86_64.tar.xz" \
    "${SHELLCHECK_SHA256}"; \
  tar -xzf git-cliff.tar.gz; \
  tar -xzf git-flow-next.tar.gz; \
  tar -xzf actionlint.tar.gz; \
  tar -xJf shellcheck.tar.xz; \
  install -D -m 0755 "git-cliff-${GIT_CLIFF_VERSION}/git-cliff" \
    /out/bin/git-cliff; \
  install -D -m 0755 git-flow /out/bin/git-flow; \
  install -D -m 0755 actionlint /out/bin/actionlint; \
  install -D -m 0755 "shellcheck-v${SHELLCHECK_VERSION}/shellcheck" \
    /out/bin/shellcheck

# Stage 1 compiler (apt.llvm.org clang) and the dependency build run. Discarded.
FROM ${UBUNTU} AS llvm
ARG DEBIAN_FRONTEND=noninteractive
ARG STAGE1_LLVM_MAJOR=23
SHELL ["/bin/bash", "-o", "errexit", "-o", "nounset", "-o", "pipefail", "-c"]
RUN readonly KEY=/etc/apt/keyrings/apt.llvm.org.asc; \
  apt-get update; \
  apt-get install -y --no-install-recommends ca-certificates curl; \
  install -d -m 0755 /etc/apt/keyrings; \
  curl -fsSLo "${KEY}" https://apt.llvm.org/llvm-snapshot.gpg.key; \
  source /etc/os-release; \
  readonly SUITE="llvm-toolchain-${VERSION_CODENAME}-${STAGE1_LLVM_MAJOR}"; \
  echo "deb [signed-by=${KEY}]" \
    "http://apt.llvm.org/${VERSION_CODENAME}/ ${SUITE} main" \
    >/etc/apt/sources.list.d/apt.llvm.org.list; \
  apt-get update; \
  apt-get install -y --no-install-recommends \
    "clang-${STAGE1_LLVM_MAJOR}" "lld-${STAGE1_LLVM_MAJOR}" \
    "llvm-${STAGE1_LLVM_MAJOR}" \
    cmake ninja-build python3 git libc6-dev \
    zlib1g-dev libzstd-dev libedit-dev libncurses-dev liblzma-dev \
    libxml2-dev; \
  rm -rf /var/lib/apt/lists/*
ENV PATH=/usr/lib/llvm-${STAGE1_LLVM_MAJOR}/bin:${PATH} \
    CC=clang-${STAGE1_LLVM_MAJOR} \
    CXX=clang++-${STAGE1_LLVM_MAJOR}
RUN --mount=type=bind,source=third_party,target=/src/third_party,rw \
  --mount=type=cache,id=umbra-toolchain-build,target=/var/cache/umbra-build,sharing=locked \
  cmake -S /src/third_party -B /tmp/dependency-build -G Ninja \
    -DUMBRA_LAYER=toolchain \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DUMBRA_STAGING_DIR=/stage \
    -DUMBRA_BUILD_CACHE_DIR=/var/cache/umbra-build \
  && cmake --build /tmp/dependency-build --target layer-toolchain

# ISPC, which compiles the kernels of Open Image Denoise's CPU device: the
# official release archive, verified by SHA256, statically linked, in
# /opt/ispc.
FROM ${UBUNTU} AS ispc
ARG DEBIAN_FRONTEND=noninteractive
ARG ISPC_VERSION=1.32.0
ARG ISPC_SHA256=faf266bda2b702d68976ffd26bbd36c1ddba43424cc3c44f25b092cd1aa1f64a
SHELL ["/bin/bash", "-o", "errexit", "-o", "nounset", "-o", "pipefail", "-c"]
RUN apt-get update \
  && apt-get install -y --no-install-recommends ca-certificates curl \
  && rm -rf /var/lib/apt/lists/*
RUN readonly ARCHIVE="ispc-v${ISPC_VERSION}-linux.tar.gz"; \
  curl -fsSLo "/tmp/${ARCHIVE}" \
    "https://github.com/ispc/ispc/releases/download/v${ISPC_VERSION}/${ARCHIVE}"; \
  echo "${ISPC_SHA256}  /tmp/${ARCHIVE}" | sha256sum -c -; \
  install -d /opt/ispc; \
  tar -xzf "/tmp/${ARCHIVE}" -C /opt/ispc --strip-components=1; \
  rm "/tmp/${ARCHIVE}"

# Python lint and format tools, pinned with hashes in docker/python-tools.txt.
FROM ${UBUNTU} AS python
ARG DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-o", "errexit", "-o", "nounset", "-o", "pipefail", "-c"]
RUN apt-get update \
  && apt-get install -y --no-install-recommends python3 python3-venv \
  && rm -rf /var/lib/apt/lists/*
RUN --mount=type=bind,source=docker/python-tools.txt,target=/tmp/python-tools.txt \
  python3 -m venv /opt/python-tools \
  && /opt/python-tools/bin/pip install --no-cache-dir \
    --disable-pip-version-check --require-hashes --only-binary :all: \
    -r /tmp/python-tools.txt

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
FROM ${UBUNTU} AS nvidia
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
SHELL ["/bin/bash", "-o", "errexit", "-o", "nounset", "-o", "pipefail", "-c"]
RUN apt-get update \
  && apt-get install -y --no-install-recommends ca-certificates curl xz-utils \
  && rm -rf /var/lib/apt/lists/*
WORKDIR /tmp/nvidia
# fetch COMPONENT VERSION SHA256 downloads the redist archive of a CUDA
# component to COMPONENT.tar.xz and verifies it.
RUN readonly REDIST=https://developer.download.nvidia.com/compute/cuda/redist; \
  readonly OPTIX=https://github.com/NVIDIA/optix-dev/archive/refs/tags; \
  fetch() { \
    local component="$1" version="$2" sha256="$3"; \
    local archive="${component}-linux-x86_64-${version}-archive.tar.xz"; \
    curl -fsSLo "${component}.tar.xz" \
      "${REDIST}/${component}/linux-x86_64/${archive}"; \
    echo "${sha256}  ${component}.tar.xz" | sha256sum -c -; \
  }; \
  fetch cuda_nvcc "${CUDA_NVCC_VERSION}" "${CUDA_NVCC_SHA256}"; \
  fetch cuda_crt "${CUDA_CRT_VERSION}" "${CUDA_CRT_SHA256}"; \
  fetch libnvvm "${LIBNVVM_VERSION}" "${LIBNVVM_SHA256}"; \
  fetch cuda_cudart "${CUDA_CUDART_VERSION}" "${CUDA_CUDART_SHA256}"; \
  fetch cccl "${CCCL_VERSION}" "${CCCL_SHA256}"; \
  fetch cuda_cuobjdump "${CUDA_CUOBJDUMP_VERSION}" \
    "${CUDA_CUOBJDUMP_SHA256}"; \
  fetch cuda_nvdisasm "${CUDA_NVDISASM_VERSION}" "${CUDA_NVDISASM_SHA256}"; \
  fetch cuda_sanitizer_api "${CUDA_SANITIZER_API_VERSION}" \
    "${CUDA_SANITIZER_API_SHA256}"; \
  fetch cuda_gdb "${CUDA_GDB_VERSION}" "${CUDA_GDB_SHA256}"; \
  fetch libcurand "${LIBCURAND_VERSION}" "${LIBCURAND_SHA256}"; \
  curl -fsSLo optix.tar.gz "${OPTIX}/v${OPTIX_VERSION}.tar.gz"; \
  echo "${OPTIX_SHA256}  optix.tar.gz" | sha256sum -c -; \
  install -d /opt/cuda/optix; \
  for component in cuda_nvcc cuda_crt libnvvm cuda_cudart cccl \
    cuda_cuobjdump cuda_nvdisasm cuda_sanitizer_api cuda_gdb; do \
    tar -xJf "${component}.tar.xz" -C /opt/cuda --strip-components=1; \
  done; \
  tar -xJf libcurand.tar.xz -C /opt/cuda --strip-components=1 \
    --wildcards '*/include/*'; \
  tar -xzf optix.tar.gz -C /opt/cuda/optix --strip-components=1; \
  rm -rf /tmp/nvidia
WORKDIR /
# nvcc links from lib64/, as in the toolkit installers; the archives use lib/.
RUN ln -s lib /opt/cuda/lib64 \
  && printf '%s\n' "CUDA ${CUDA_VERSION}, OptiX ${OPTIX_VERSION}" \
    >/opt/cuda/version.txt

FROM ${UBUNTU} AS toolchain
ARG DEBIAN_FRONTEND=noninteractive
SHELL ["/bin/bash", "-o", "errexit", "-o", "nounset", "-o", "pipefail", "-c"]
# Build tools and the C-ABI libraries the toolchain links (zlib, zstd, libedit,
# ncurses, lzma, libxml2). opt-viewer needs python3-yaml and python3-pygments.
# linux-perf is the sampling profiler, which also records the profiles BOLT
# (llvm-bolt, perf2bolt) optimizes with; unlike linux-tools-*, it does not
# depend on the kernel version, so it runs on any host kernel. Profiling in a
# rootless container needs the host's kernel.perf_event_paranoid at 2 or lower.
# openssh-client provides ssh-keygen, which git runs to sign commits and tags
# with gpg.format ssh (through the forwarded ssh-agent) and to verify them.
# git-lfs fetches the files a submodule keeps in Git LFS (Open Image Denoise's
# weights); its filters are set up for every repository, so that git checks
# them out and git status works in such a submodule.
RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    ca-certificates cmake ninja-build python3 python3-yaml python3-pygments \
    git git-lfs openssh-client \
    libc6-dev zlib1g libzstd1 libedit2 libncursesw6 libncurses6 liblzma5 \
    libxml2-16 linux-perf \
  && rm -rf /var/lib/apt/lists/* \
  && git lfs install --system --skip-repo
COPY --from=llvm /stage/ /
# The license of the LLVM libraries that umbra's install rules ship (libc++,
# libc++abi, libunwind and libLLVM): they take it from
# share/licenses/llvm-project/.
RUN --mount=type=bind,source=third_party/llvm-project/LICENSE.TXT,target=/tmp/LICENSE.TXT \
  install -D -m 0644 /tmp/LICENSE.TXT \
    /usr/local/share/licenses/llvm-project/LICENSE.TXT
COPY --from=build /out/bin/ /usr/local/bin/
COPY --from=ispc /opt/ispc/ /opt/ispc/
COPY --from=python /opt/python-tools /opt/python-tools
COPY --from=nvidia /opt/cuda/ /opt/cuda/
RUN ln -s clang /usr/local/bin/cc \
  && ln -s clang++ /usr/local/bin/c++ \
  && ldconfig
# The Python tools' venv comes last on PATH, so python3 stays the system one
# (with python3-yaml and python3-pygments for opt-viewer).
ENV PATH=/opt/cuda/bin:/opt/ispc/bin:${PATH}:/opt/python-tools/bin \
    CC=clang \
    CXX=clang++ \
    CUDAToolkit_ROOT=/opt/cuda \
    OptiX_INSTALL_DIR=/opt/cuda/optix
RUN nvcc --version \
  && cuobjdump --version \
  && cuda-gdb --version \
  && compute-sanitizer --version \
  && ispc --version
