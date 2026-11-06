# Umbra

[![CI](https://github.com/sergioffpc/umbra/actions/workflows/ci.yml/badge.svg?branch=develop)](https://github.com/sergioffpc/umbra/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Umbra is a physically based ray tracer written in Rust, inspired by
[pbrt-v4](https://github.com/mmp/pbrt-v4) and the book
[_Physically Based Rendering: From Theory to Implementation_](https://pbr-book.org/).

It is designed to render on CPUs only, use every core of a machine, and spread
the work of one image over the machines of a LAN.

> **Status:** early development. Nothing renders yet.

## Building

Requires Rust 1.99.1, pinned in `rust-toolchain.toml`; rustup installs it automatically.

```sh
git clone https://github.com/sergioffpc/umbra.git
cd umbra
cargo build --release
```

## Usage

```sh
cargo run --release
```

## Container image

Every release publishes a minimal [chiseled Ubuntu](https://ubuntu.com/containers/chiselled)
image to the GitHub Container Registry: no shell, no package manager, running
as the non-root user `umbra` (uid 10001) in `/work`. It is built for
`linux/amd64` with the `x86-64-v3` baseline (AVX2 and FMA: Intel Haswell, AMD Zen
or newer).

```sh
docker pull ghcr.io/sergioffpc/umbra:latest
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/work" ghcr.io/sergioffpc/umbra:latest
```

`--user` lets `umbra` write its output into the mounted directory with your
own ownership.

Tags: `X.Y.Z` for a release, `X.Y` for the latest patch of a minor version,
`latest` for the newest release. Verify an image's build provenance with:

```sh
gh attestation verify oci://ghcr.io/sergioffpc/umbra:latest -R sergioffpc/umbra
```

## Documentation

The architecture (arc42), the architecture decision records and the glossary
are indexed in [`docs/`](docs/README.md).

## Contributing

Contributions are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) for the
branching model, commit conventions and checks to run before opening a pull
request. This project follows a [Code of Conduct](CODE_OF_CONDUCT.md).

To report a security issue, see [SECURITY.md](SECURITY.md).

## License

Umbra is licensed under the [MIT License](LICENSE).
