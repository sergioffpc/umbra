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

## Install

Each release of Umbra is a container image, `ghcr.io/sergioffpc/umbra`,
tagged `X.Y.Z`, `X.Y`, `X` and `latest`, for Linux on x86-64 CPUs with AVX2,
FMA and BMI2 (`x86-64-v3`). Its entrypoint is `umbra`, which runs as user and
group 65532 in `/work`, the directory to mount scenes and outputs on:

```sh
docker run --rm -v "$PWD:/work" ghcr.io/sergioffpc/umbra:X.Y.Z devices
docker run --rm -v "$PWD:/work" ghcr.io/sergioffpc/umbra:X.Y.Z render
```

Without a command, `umbra` shows its help. With the NVIDIA Container Toolkit
installed, `--gpus all` gives the container the GPU and its driver, which the
GPU module loads:

```sh
docker run --rm --gpus all -v "$PWD:/work" ghcr.io/sergioffpc/umbra:X.Y.Z \
  render --devices=gpu0
```

On a machine with several NUMA nodes, each CPU device keeps its memory on its
node by binding the memory policy of its threads, which Docker's default
seccomp profile allows only with `--cap-add SYS_NICE`. Without it, umbra
warns once per device and renders with its memory where the kernel places it.

The image holds the license texts of Umbra and of every component in it in
`/opt/umbra/share/licenses/`, and the copyright file of each Ubuntu package
in `/usr/share/doc/<package>/`.

### Verify the image

The release workflow signs the image with cosign, keyless, under its own
identity, and attests its SLSA build provenance and its SPDX SBOM. To check
the signature, with [cosign](https://docs.sigstore.dev/cosign/):

```sh
cosign verify ghcr.io/sergioffpc/umbra:X.Y.Z \
  --certificate-identity-regexp \
  '^https://github\.com/sergioffpc/umbra/\.github/workflows/release\.yml@refs/tags/v' \
  --certificate-oidc-issuer https://token.actions.githubusercontent.com
```

To check the build provenance, and then the SBOM, with the
[GitHub CLI](https://cli.github.com/):

```sh
gh attestation verify oci://ghcr.io/sergioffpc/umbra:X.Y.Z \
  --repo sergioffpc/umbra
gh attestation verify oci://ghcr.io/sergioffpc/umbra:X.Y.Z \
  --repo sergioffpc/umbra --predicate-type https://spdx.dev/Document/v2.3
```

The SBOM lists the Ubuntu packages of the image with their versions, Umbra,
and the third-party components built into it with their versions and
licenses.

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
   python3 -m forge build   # debug build
   python3 -m forge check   # build and run the tests
   ```

Run `python3 -m forge --help` for the full list of commands: `build`,
`check`, `check-asan`, `check-tsan`, `check-rtsan`, `check-all`,
`coverage`, `format`,
`lint`, `clean` and `distclean`.

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
| `forge/` | Developer commands over the CMake presets and the checks |
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
