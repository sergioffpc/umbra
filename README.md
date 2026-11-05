# Umbra

[![CI](https://github.com/sergioffpc/umbra/actions/workflows/ci.yml/badge.svg?branch=develop)](https://github.com/sergioffpc/umbra/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Umbra is a physically based ray tracer written in Rust, inspired by
[pbrt-v4](https://github.com/mmp/pbrt-v4) and the book
[*Physically Based Rendering: From Theory to Implementation*](https://pbr-book.org/).

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

## Contributing

Contributions are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) for the
branching model, commit conventions and checks to run before opening a pull
request. This project follows a [Code of Conduct](CODE_OF_CONDUCT.md).

To report a security issue, see [SECURITY.md](SECURITY.md).

## License

Umbra is licensed under the [MIT License](LICENSE).
