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
