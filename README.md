# Umbra

Umbra is a physically based path tracer written in C++23.

The project aims to give C++ as many Rust-like guarantees as the language
allows: memory, thread, lifetime, nullability, integer and conversion safety,
and explicit ownership, all enforced by the compiler, clang-tidy, clang-query,
git hooks and CI. It uses the LLVM ecosystem only (clang, libc++, compiler-rt,
libunwind, lld, lldb and LLVM tooling); no GCC runtime is loaded by any of its
processes.

## Status

Umbra is pre-1.0 and under active development. The current scaffold builds a
`umbra` executable that prints its version information. **Until 1.0.0, any
release may break the API**: breaking changes bump the minor version (see
[Versioning](CONTRIBUTING.md#versioning-and-releases)).

## Supported environment

**The devcontainer is the only supported environment.** Builds outside it are
not supported, and issues that cannot be reproduced inside it will be closed.

The devcontainer pulls a prebuilt image from the GitHub Container Registry.
That image contains LLVM 23.1.6 built from source with libc++, compiler-rt and
libunwind, plus the third-party dependencies and the NVIDIA CUDA and OptiX
build tools, so you never compile the toolchain locally. Only Linux on x86-64
is targeted; the default architecture level is `x86-64-v3`. An NVIDIA GPU is
optional (RTX 5070 Ti class or newer); see
[GPU hosts](CONTRIBUTING.md#gpu-hosts).

## Getting started

1. Install rootless Podman and VS Code with the Dev Containers extension, and
   set `"dev.containers.dockerPath": "podman"` in your VS Code user settings.
   The image is a private package: log in with
   `podman login ghcr.io --compat-auth-file ~/.docker/config.json` (a personal
   access token with `read:packages`).
2. Clone the repository and open it in the devcontainer
   ("Dev Containers: Reopen in Container").
3. The container's create step installs the git hooks, enables commit
   signing, configures the debug CMake preset, and installs `gh` and Claude
   Code. See [CONTRIBUTING.md](CONTRIBUTING.md#environment).
4. Build and test:

   ```sh
   make          # debug build
   make test     # build and run the tests
   ```

Run `make help` for the full list of targets: `all`, `test`, `release`,
`asan`, `tsan`, `format`, `lint`, `checks`, `clean` and `help`.

## Repository layout

| Path | Contents |
| --- | --- |
| `apps/umbra/` | Entry point of the `umbra` executable only |
| `include/umbra/<module>/` | Public headers of each module |
| `src/<module>/` | Sources and private headers of each module |
| `include/umbra/ext/`, `src/ext/` | Bridges to third-party libraries that need RTTI or exceptions |
| `tests/<module>/` | GoogleTest tests, mirroring the module tree |
| `benchmarks/` | Google Benchmark benchmarks |
| `cmake/` | CMake modules (compiler, warnings, safety, sanitizers, hardening, build info) |
| `checks/` | Validators shared by the git hooks and CI, and clang-query rules |
| `sanitizers/` | Sanitizer suppressions and default options |
| `third_party/` | Submodules, dependency build, patches and the versions manifest |
| `docker/` | Container image definitions |
| `.devcontainer/` | Devcontainer configuration |
| `.githooks/` | Python git hooks |
| `.github/` | Workflows, issue forms and repository metadata |

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request, and
follow the [Code of Conduct](CODE_OF_CONDUCT.md). Report security
vulnerabilities as described in [SECURITY.md](SECURITY.md). Notable changes
are listed in [CHANGELOG.md](CHANGELOG.md).

## License

Copyright The Umbra Authors. Licensed under the
[Apache License, Version 2.0](LICENSE). See [NOTICE](NOTICE) and
[AUTHORS](AUTHORS).
