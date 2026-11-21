# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# umbra-toolchain: Ubuntu 26.04 with LLVM 23 built from the llvm-project
# submodule by the third-party superbuild (layer "toolchain"), plus the C-ABI
# libraries and build tools that come from Ubuntu, release tools (git-cliff,
# git-flow-next, actionlint) and the Python lint and format tools.
#
# Build from the repository root with the submodules checked out:
#   docker buildx build -f docker/toolchain.Dockerfile .
#
# The LLVM build tree lives in the cache mount "umbra-toolchain-build" so that
# an interrupted build resumes; the deps workflow persists it between runs.

ARG UBUNTU=ubuntu:26.04

# Stage 1 compiler (apt.llvm.org clang) and the superbuild run. Discarded.
FROM ${UBUNTU} AS llvm
ARG DEBIAN_FRONTEND=noninteractive
ARG STAGE1_LLVM_MAJOR=23
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl \
 && install -d -m 0755 /etc/apt/keyrings \
 && curl -fsSLo /etc/apt/keyrings/apt.llvm.org.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
 && . /etc/os-release \
 && echo "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.asc] http://apt.llvm.org/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME}-${STAGE1_LLVM_MAJOR} main" \
      > /etc/apt/sources.list.d/apt.llvm.org.list \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      clang-${STAGE1_LLVM_MAJOR} lld-${STAGE1_LLVM_MAJOR} llvm-${STAGE1_LLVM_MAJOR} \
      cmake ninja-build python3 git libc6-dev \
      zlib1g-dev libzstd-dev libedit-dev libncurses-dev liblzma-dev libxml2-dev \
 && rm -rf /var/lib/apt/lists/*
ENV PATH=/usr/lib/llvm-${STAGE1_LLVM_MAJOR}/bin:${PATH} \
    CC=clang-${STAGE1_LLVM_MAJOR} \
    CXX=clang++-${STAGE1_LLVM_MAJOR}
RUN --mount=type=bind,source=third_party,target=/src/third_party,rw \
    --mount=type=cache,id=umbra-toolchain-build,target=/var/cache/umbra-build,sharing=locked \
    cmake -S /src/third_party -B /tmp/superbuild -G Ninja \
          -DUMBRA_LAYER=toolchain \
          -DCMAKE_INSTALL_PREFIX=/usr/local \
          -DUMBRA_STAGING_DIR=/stage \
          -DUMBRA_BUILD_CACHE_DIR=/var/cache/umbra-build \
 && cmake --build /tmp/superbuild --target layer-toolchain

# Release binaries of git-cliff, git-flow-next and actionlint, verified by
# SHA256. All three are statically linked and need no GCC runtime (for
# git-cliff that is the musl build).
FROM ${UBUNTU} AS tools
ARG DEBIAN_FRONTEND=noninteractive
ARG GIT_CLIFF_VERSION=2.14.2
ARG GIT_CLIFF_SHA256=fa21e2fc70046fcfd0e43e5e4613873ab799aff3d9dc8559c00d31b270e417d0
ARG GIT_FLOW_NEXT_VERSION=2.1.0
ARG GIT_FLOW_NEXT_SHA256=e335b5818f2b2f820c17844c194030d0db78f3cdfa9bb999fcb9ce092ba9ecd2
ARG ACTIONLINT_VERSION=1.7.12
ARG ACTIONLINT_SHA256=8aca8db96f1b94770f1b0d72b6dddcb1ebb8123cb3712530b08cc387b349a3d8
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /tmp/tools
RUN curl -fsSLo git-cliff.tar.gz \
      "https://github.com/orhun/git-cliff/releases/download/v${GIT_CLIFF_VERSION}/git-cliff-${GIT_CLIFF_VERSION}-x86_64-unknown-linux-musl.tar.gz" \
 && echo "${GIT_CLIFF_SHA256}  git-cliff.tar.gz" | sha256sum -c - \
 && curl -fsSLo git-flow-next.tar.gz \
      "https://github.com/gittower/git-flow-next/releases/download/v${GIT_FLOW_NEXT_VERSION}/git-flow-next-v${GIT_FLOW_NEXT_VERSION}-linux-amd64.tar.gz" \
 && echo "${GIT_FLOW_NEXT_SHA256}  git-flow-next.tar.gz" | sha256sum -c - \
 && curl -fsSLo actionlint.tar.gz \
      "https://github.com/rhysd/actionlint/releases/download/v${ACTIONLINT_VERSION}/actionlint_${ACTIONLINT_VERSION}_linux_amd64.tar.gz" \
 && echo "${ACTIONLINT_SHA256}  actionlint.tar.gz" | sha256sum -c - \
 && tar -xzf git-cliff.tar.gz \
 && tar -xzf git-flow-next.tar.gz \
 && tar -xzf actionlint.tar.gz \
 && install -D -m 0755 "git-cliff-${GIT_CLIFF_VERSION}/git-cliff" /out/bin/git-cliff \
 && install -D -m 0755 git-flow /out/bin/git-flow \
 && install -D -m 0755 actionlint /out/bin/actionlint

# Python lint and format tools, pinned with hashes in docker/python-tools.txt.
FROM ${UBUNTU} AS python-tools
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends python3 python3-venv \
 && rm -rf /var/lib/apt/lists/*
RUN --mount=type=bind,source=docker/python-tools.txt,target=/tmp/python-tools.txt \
    python3 -m venv /opt/python-tools \
 && /opt/python-tools/bin/pip install --no-cache-dir --disable-pip-version-check \
      --require-hashes --only-binary :all: -r /tmp/python-tools.txt

FROM ${UBUNTU} AS toolchain
ARG DEBIAN_FRONTEND=noninteractive
# Build tools and the C-ABI libraries the toolchain links (zlib, zstd, libedit,
# ncurses, lzma, libxml2). opt-viewer needs python3-yaml and python3-pygments.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates cmake ninja-build make python3 python3-yaml python3-pygments git \
      libc6-dev zlib1g libzstd1 libedit2 libncursesw6 liblzma5 libxml2-16 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=llvm /stage/ /
COPY --from=tools /out/bin/ /usr/local/bin/
COPY --from=python-tools /opt/python-tools /opt/python-tools
RUN ln -s clang /usr/local/bin/cc \
 && ln -s clang++ /usr/local/bin/c++ \
 && ldconfig
ENV PATH=/opt/python-tools/bin:${PATH} \
    CC=clang \
    CXX=clang++
