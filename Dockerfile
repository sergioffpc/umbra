# syntax=docker/dockerfile:1
#
# Release container image for umbra: a chiseled Ubuntu 26.04 LTS runtime (no
# shell, no package manager, non-root) holding only the `umbra` binary and the
# glibc and libgcc slices it links against. Built and published to
# ghcr.io/sergioffpc/umbra by .github/workflows/release-image.yml on every
# vX.Y.Z tag, once per Linux target CPU (X.Y.Z-znver2, X.Y.Z-broadwell); see
# docs/adr/0005-chiseled-release-container-image.md and
# docs/adr/0012-per-cpu-binaries-locked-to-one-commit.md.
#
# The published images are linux/amd64 only. Build one locally with:
#   docker build --build-arg TARGET_CPU=znver2 -t umbra:znver2 .
# TARGET_CPU is required, and only linux/amd64 builds (ADR 0023).

# ---------------------------------------------------------------------------
# Builder: the toolchain pinned in rust-toolchain.toml, `cargo build --release`.
# Both stages use the same Ubuntu 26.04 LTS image, so the binary is linked
# against the glibc the runtime slices ship. Dependabot updates the digest in
# both FROM lines; keep them identical.
FROM ubuntu:26.04@sha256:6a06111c3b0a8600feadb270aec771dc239bf407b25625f93f44ab00e11f4d3c AS builder

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# Build-only packages, not shipped: unpinned, since the Ubuntu archive drops
# superseded versions and a pin would break the build.
# hadolint ignore=DL3008
RUN apt-get update \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y --no-install-recommends ca-certificates curl gcc libc6-dev \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

ENV RUSTUP_HOME=/usr/local/rustup \
    CARGO_HOME=/usr/local/cargo \
    PATH=/usr/local/cargo/bin:${PATH}

# rustup only; the toolchain itself comes from rust-toolchain.toml below, so a
# toolchain bump there rebuilds this image with the new compiler.
RUN curl -fsSL --proto '=https' --tlsv1.2 https://sh.rustup.rs \
        | sh -s -- -y --no-modify-path --profile minimal --default-toolchain none

WORKDIR /src
COPY rust-toolchain.toml ./
RUN rustup toolchain install && rustc --version

# Target CPU, passed to rustc as `-C target-cpu=${TARGET_CPU}`: one image per
# Linux Worker CPU (znver2 for the P620, broadwell for the R630), see
# docs/architecture/07-deployment-view.md. There is no generic x86-64 image, so
# the build fails without it, and Umbra builds for linux/amd64 only (ADR 0023).
# RUSTFLAGS replaces the `target-cpu=native` set in .cargo/config.toml, which
# .dockerignore also keeps out of the build context. The CPU must include
# x86-64-v3 (FMA, SSE4.1, AVX2), or umbra-math stops the build (ADR 0023).
ARG TARGETARCH
ARG TARGET_CPU=""

COPY . .
RUN --mount=type=cache,target=/usr/local/cargo/registry \
    --mount=type=cache,target=/src/target \
    if [ "${TARGETARCH:-amd64}" != amd64 ]; then \
        echo "error: Umbra builds for linux/amd64 only (ADR 0023)" >&2; exit 1; \
    fi; \
    if [ -z "${TARGET_CPU}" ]; then \
        echo "error: pass --build-arg TARGET_CPU=<cpu> (znver2 or broadwell)" >&2; exit 1; \
    fi \
    && export RUSTFLAGS="-C target-cpu=${TARGET_CPU}" \
    && echo "RUSTFLAGS=${RUSTFLAGS}" \
    && cargo build --release --locked \
    && install -D -m 0755 target/release/umbra /out/usr/bin/umbra

# ---------------------------------------------------------------------------
# Chisel: cut the runtime root filesystem from Ubuntu 26.04 slices.
FROM ubuntu:26.04@sha256:6a06111c3b0a8600feadb270aec771dc239bf407b25625f93f44ab00e11f4d3c AS chisel

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# Chisel is not packaged by Ubuntu; fetch the release binary and check it
# against the SHA-384 published with it. Bump the version and the sum
# together (Dependabot does not track these).
ARG CHISEL_VERSION=v1.5.1
ARG CHISEL_SHA384_AMD64=89612f46de83e850aae11fca26a32a22cc2964891f2cc538211b906787d2b83e4496fa203a5074e06a9932f1fbe84a73
ARG TARGETARCH

# hadolint ignore=DL3008
RUN apt-get update \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y --no-install-recommends ca-certificates curl \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

RUN set -eu; \
    case "${TARGETARCH:-amd64}" in \
        amd64) sum="${CHISEL_SHA384_AMD64}"; arch=amd64 ;; \
        *) echo "unsupported architecture: ${TARGETARCH}" >&2; exit 1 ;; \
    esac; \
    tarball="chisel_${CHISEL_VERSION}_linux_${arch}.tar.gz"; \
    curl -fsSL --proto '=https' --tlsv1.2 -o "/tmp/${tarball}" \
        "https://github.com/canonical/chisel/releases/download/${CHISEL_VERSION}/${tarball}"; \
    echo "${sum}  /tmp/${tarball}" | sha384sum -c -; \
    tar -xzf "/tmp/${tarball}" -C /usr/local/bin chisel; \
    rm "/tmp/${tarball}"

# Slices umbra needs at run time:
#   base-files_base          FHS skeleton (/etc, /tmp, /usr, ...)
#   base-files_release-info  /etc/os-release, for scanners
#   base-files_chisel        /var/lib/chisel manifest, for SBOM tools
#   base-passwd_data         /etc/passwd and /etc/group
#   libc6_libs               glibc (the binary targets x86_64-unknown-linux-gnu)
#   libgcc-s1_libs           libgcc_s, which Rust's std links on linux-gnu
# Add slices here when umbra gains a dependency on a system library.
RUN mkdir /rootfs \
    && chisel cut --release ubuntu-26.04 --root /rootfs \
        base-files_base \
        base-files_release-info \
        base-files_chisel \
        base-passwd_data \
        libc6_libs \
        libgcc-s1_libs

# Non-root user and its working directory, where scenes are mounted.
# The id is repeated in the runtime stage's USER.
RUN echo "umbra:x:10001:10001:umbra:/work:/usr/sbin/nologin" >> /rootfs/etc/passwd \
    && echo "umbra:x:10001:" >> /rootfs/etc/group \
    && install -d -m 0755 -o 10001 -g 10001 /rootfs/work

# ---------------------------------------------------------------------------
# Runtime: the chiseled root filesystem plus the binary.
FROM scratch

COPY --from=chisel /rootfs /
COPY --from=builder /out /

USER 10001:10001
WORKDIR /work
ENTRYPOINT ["/usr/bin/umbra"]
