# Dev container image for umbra: Debian 13 (trixie), the same release as the
# R630 Worker, with the toolchain pinned in rust-toolchain.toml and every tool
# the git hooks and CI use. Built by .devcontainer/devcontainer.json with the
# repository root as context; Dockerfile.dockerignore limits that context to
# the files copied below. x86-64 only (ADR 0023).
FROM mcr.microsoft.com/devcontainers/rust:1-trixie

# Native packages: shellcheck (shell scripts), run by the pre-commit hook and
# scripts/check.sh. The base image already has build-essential, git, curl and
# lldb.
RUN apt-get update \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y --no-install-recommends shellcheck \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Everything below is installed as the dev user, who owns the Rust install
# (RUSTUP_HOME and CARGO_HOME are group-writable for `rustlang`).
USER vscode

# Toolchain pinned in rust-toolchain.toml, plus rust-src for rust-analyzer.
# Copying the file means a toolchain bump rebuilds this layer.
COPY --chown=vscode:vscode rust-toolchain.toml /tmp/umbra/rust-toolchain.toml
RUN cd /tmp/umbra \
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
