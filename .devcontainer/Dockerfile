# Dev container image for umbra: Ubuntu 26.04 LTS, the release the root
# Dockerfile builds and ships on, with the toolchain pinned in
# rust-toolchain.toml and every tool the git hooks and CI use. Built by
# .devcontainer/devcontainer.json with the repository root as context; the
# root .dockerignore limits that context to the files copied below (the
# features build a generated copy of this Dockerfile, so a
# Dockerfile.dockerignore beside it would be ignored). x86-64 only (ADR 0023).
#
# The devcontainers Rust image is Debian only, so this starts from the Ubuntu
# base image (git, curl, sudo and the vscode user) and installs rustup itself.
FROM mcr.microsoft.com/devcontainers/base:3-ubuntu26.04

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# Native packages: the C toolchain and linker, lldb (debugger), python3
# (scripts/) and shellcheck (shell scripts, run by the pre-commit hook and
# scripts/check.sh).
RUN apt-get update \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y --no-install-recommends build-essential lldb python3 shellcheck \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# CUDA's libdevice.10.bc, the one file of CUDA that building the GPU kernels
# to run needs (umbra-gpu's build.rs, the GPU tests): NVIDIA's C math in
# LLVM bitcode. It is taken from NVIDIA's nvidia-cuda-nvcc-cu12 wheel on PyPI,
# checked against the SHA-256 PyPI publishes, instead of installing the CUDA
# toolkit, and goes where the build script looks by default. The driver and
# OptiX come from the host, mounted by the NVIDIA Container Toolkit. Bump the
# URL and the sum together.
ARG LIBDEVICE_WHEEL_URL=https://files.pythonhosted.org/packages/25/48/b54a06168a2190572a312bfe4ce443687773eb61367ced31e064953dd2f7/nvidia_cuda_nvcc_cu12-12.9.86-py3-none-manylinux2010_x86_64.manylinux_2_12_x86_64.whl
ARG LIBDEVICE_WHEEL_SHA256=5d6a0d32fdc7ea39917c20065614ae93add6f577d840233237ff08e9a38f58f0
RUN curl -fsSL --proto '=https' --tlsv1.2 -o /tmp/nvcc.whl "${LIBDEVICE_WHEEL_URL}" \
    && echo "${LIBDEVICE_WHEEL_SHA256}  /tmp/nvcc.whl" | sha256sum -c - \
    && install -d /usr/local/cuda/nvvm/libdevice \
    && python3 -I -c 'import sys, zipfile; sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read("nvidia/cuda_nvcc/nvvm/libdevice/libdevice.10.bc"))' \
        /tmp/nvcc.whl > /usr/local/cuda/nvvm/libdevice/libdevice.10.bc \
    && rm /tmp/nvcc.whl

# Same layout as the root Dockerfile and the former Rust image: the cargo
# cache volumes in devcontainer.json mount under /usr/local/cargo.
ENV RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:${PATH}

# The dev user owns the Rust install, so rustup and cargo install work
# without sudo.
RUN install -d -o vscode -g vscode "${RUSTUP_HOME}" "${CARGO_HOME}"

# Everything below is installed as the dev user.
USER vscode

# rustup, then the toolchain pinned in rust-toolchain.toml plus rust-src for
# rust-analyzer. Copying the file means a toolchain bump rebuilds this layer.
COPY --chown=vscode:vscode rust-toolchain.toml /tmp/umbra/rust-toolchain.toml
RUN curl -fsSL --proto '=https' --tlsv1.2 https://sh.rustup.rs \
        | sh -s -- -y --no-modify-path --profile minimal --default-toolchain none \
    && cd /tmp/umbra \
    && rustup toolchain install \
    && rustup component add rust-src \
    && rustup default "$(rustup show active-toolchain | cut -d' ' -f1)" \
    && rm -rf /tmp/umbra

# The tools the hooks, CI and CONTRIBUTING.md use, at the versions pinned in
# scripts/install-tools.sh. Prettier and markdownlint-cli2 need npm, which the
# Node.js feature installs after this Dockerfile, so post-create.sh installs
# them with the same script.
COPY --chown=vscode:vscode scripts/install-tools.sh /tmp/umbra/install-tools.sh
RUN /tmp/umbra/install-tools.sh cargo-nextest cargo-llvm-cov cargo-deny cargo-shear \
        typos git-cliff actionlint editorconfig-checker taplo \
    && rm -rf /tmp/umbra "${CARGO_HOME}/registry" "${CARGO_HOME}/git"
