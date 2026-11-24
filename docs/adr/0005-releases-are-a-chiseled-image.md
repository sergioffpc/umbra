# Releases are a chiseled OCI image

A release of umbra is one container image, `ghcr.io/sergioffpc/umbra`, and
nothing else. umbra loads about fifty shared libraries: its own toolchain's
(libc++, libunwind, LLVM), the C++ dependencies built from `third_party/`,
and C libraries from Ubuntu. An image ships them together, in versions that
were tested together, where an archive would have to match the glibc and
the libraries of each system it lands on.

```
docker run --rm --gpus all -v "$PWD:/work" ghcr.io/sergioffpc/umbra:0.2.0 \
    render --devices=gpu0 ...
```

The image is chiseled: its Ubuntu libraries are cut with Canonical's Chisel
from the packages of the Ubuntu release the build uses, slice by slice, so
it holds the files umbra loads and their copyright notices, with no shell,
no package manager and no other tools. The Chisel slices come from
`canonical/chisel-releases`. A package that has no slice there gets one of
our own, in `docker/chisel/slices/`, and a pull request upstream; ours is
deleted once upstream has it.

Of the libraries umbra builds, the image takes those that `ldd` finds for
`umbra` and the GPU module, collected when the image is built, and an
explicit list of what umbra loads with `dlopen`, which `ldd` cannot see: the
GPU module itself and OpenUSD's plugins. CI checks that the image starts:
`umbra version`, `umbra devices` and `umbra render`, all of which initialize
every library. CI has no GPU, so the GPU module's use of the driver's CUDA
and OptiX, which the NVIDIA Container Toolkit mounts into the container, is
not verified in the image.

The image's contract:

- `/opt/umbra/bin/umbra`, and `/opt/umbra/lib/` with the GPU module, the
  libraries and `usd/` for the plugins. Binaries find their libraries
  through `RUNPATH=$ORIGIN/../lib`, without `LD_LIBRARY_PATH`.
- `ENTRYPOINT ["/opt/umbra/bin/umbra"]` and `CMD ["--help"]`.
- An unprivileged user, UID and GID 65532, and `WORKDIR /work`, where scenes
  and outputs are mounted.
- The license texts of umbra and of everything in the image in
  `/opt/umbra/share/licenses/<component>/`.

The image is `linux/amd64` only, built for `x86-64-v3` (AVX2, FMA, BMI2), as
every build is. umbra checks the CPU before anything else and stops with a
fatal error on one without it, rather than with an illegal instruction.

A signed `vX.Y.Z` tag on `main` builds and publishes the image, tagged
`X.Y.Z`, `X.Y`, `X` and `latest`. Release and hotfix pull requests build and
check it without publishing, so that a missing slice or library shows up
before the tag. The published image is signed with cosign, keyless, under
the identity of the workflow, and carries a SLSA build provenance
attestation and an SPDX SBOM, which lists the Ubuntu packages, the
submodules and their licenses.

## Consequences

- CMake gains `install()` rules that lay out `bin/`, `lib/` and
  `share/licenses/` with the relative RUNPATH; the image copies that tree.
- A dependency that adds a library from Ubuntu needs its slice; one loaded
  with `dlopen` needs to be added to the explicit list.
- Users without containers have no release to install; an archive of the
  same tree can be added if someone needs it.
