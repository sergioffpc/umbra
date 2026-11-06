# Dev container image for umbra: Debian 13 (trixie), the same release as the
# R630 Worker, with the toolchain pinned in rust-toolchain.toml and every tool
# the git hooks and CI use. Built by .devcontainer/devcontainer.json with the
# repository root as context; Dockerfile.dockerignore limits that context to
# the files copied below.
FROM mcr.microsoft.com/devcontainers/rust:1-trixie

# Native packages: Git LFS (images and meshes, required by the git hooks),
# shellcheck (shell scripts) and yamllint (YAML), both run by the pre-commit
# hook. The base image already has build-essential, git, curl and lldb.
RUN apt-get update \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y --no-install-recommends git-lfs shellcheck yamllint \
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

# Cargo tools used by the hooks, CI and CONTRIBUTING.md, installed as
# prebuilt binaries with cargo-binstall. Bump the versions here.
ARG BINSTALL_VERSION=1.26.2
ARG NEXTEST_VERSION=0.9.151
ARG LLVM_COV_VERSION=0.9.1
ARG DENY_VERSION=0.20.2
ARG SHEAR_VERSION=1.14.0
ARG TYPOS_VERSION=1.52.0
ARG GIT_CLIFF_VERSION=2.14.2
RUN curl -fsSL --proto '=https' --tlsv1.2 \
        "https://github.com/cargo-bins/cargo-binstall/releases/download/v${BINSTALL_VERSION}/cargo-binstall-$(uname -m)-unknown-linux-musl.tgz" \
        | tar -xz -C "${CARGO_HOME}/bin" cargo-binstall \
    && cargo binstall --no-confirm --locked --disable-telemetry \
        "cargo-nextest@${NEXTEST_VERSION}" \
        "cargo-llvm-cov@${LLVM_COV_VERSION}" \
        "cargo-deny@${DENY_VERSION}" \
        "cargo-shear@${SHEAR_VERSION}" \
        "typos-cli@${TYPOS_VERSION}" \
        "git-cliff@${GIT_CLIFF_VERSION}" \
    && rm -rf "${CARGO_HOME}/registry" "${CARGO_HOME}/git"

# Linters run by the pre-commit hook that are not cargo crates: prebuilt
# release binaries for the container's architecture (amd64 or arm64).
# Keep the versions in sync with the `lint` job in .github/workflows/ci.yml.
ARG ACTIONLINT_VERSION=1.7.12
ARG EDITORCONFIG_CHECKER_VERSION=4.0.2
ARG TAPLO_VERSION=0.10.0
RUN arch="$(dpkg --print-architecture)" \
    && curl -fsSL --proto '=https' --tlsv1.2 \
        "https://github.com/tamasfe/taplo/releases/download/${TAPLO_VERSION}/taplo-linux-$(uname -m).gz" \
        | gunzip > "${CARGO_HOME}/bin/taplo" \
    && chmod +x "${CARGO_HOME}/bin/taplo" \
    && curl -fsSL --proto '=https' --tlsv1.2 \
        "https://github.com/rhysd/actionlint/releases/download/v${ACTIONLINT_VERSION}/actionlint_${ACTIONLINT_VERSION}_linux_${arch}.tar.gz" \
        | tar -xz -C "${CARGO_HOME}/bin" actionlint \
    && curl -fsSL --proto '=https' --tlsv1.2 \
        "https://github.com/editorconfig-checker/editorconfig-checker/releases/download/v${EDITORCONFIG_CHECKER_VERSION}/editorconfig-checker-linux-${arch}.tar.gz" \
        | tar -xz -C "${CARGO_HOME}/bin" editorconfig-checker
