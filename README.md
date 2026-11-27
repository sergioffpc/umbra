# Umbra

Umbra is a physically based, spectral path tracer for USD scenes. It renders
through Hydra, so it reads what a USD stage holds as Hydra syncs it, and it
renders on every CPU NUMA node and NVIDIA GPU of the machine at once, each
from its own memory. Every color it reads is converted into ACEScg.
Lights are physical: every light lights everything, and the artistic
controls USD offers are ignored, with a warning. Each render product is
written as an OpenEXR file, its color can be denoised with Open Image
Denoise, and the frames can be watched in [tev](https://github.com/Tom94/tev)
as they render.

## Status

Umbra is pre-1.0 and under active development. It reads a scene, commits it
to its CPU and GPU devices and writes the render products, denoised color
included, but its integrator is a placeholder: the `depth` and `normal`
render vars hold the scene's depth and normals, while `color` shows the
direction of the camera rays and `albedo` is 0. **Until 1.0.0, any release
may break compatibility**: breaking changes bump the minor version.

## Install

Each release of Umbra is a container image, `ghcr.io/sergioffpc/umbra`,
tagged `X.Y.Z`, `X.Y`, `X` and `latest`, for Linux on x86-64 CPUs with AVX2,
FMA and BMI2 (`x86-64-v3`). Its entrypoint is `umbra`, which runs as user and
group 65532 in `/work`, the directory to mount scenes and outputs on:

```sh
docker run --rm -v "$PWD:/work" ghcr.io/sergioffpc/umbra:X.Y.Z devices
docker run --rm -v "$PWD:/work" ghcr.io/sergioffpc/umbra:X.Y.Z \
  render scene.usda
```

With the NVIDIA Container Toolkit installed, `--gpus all` gives the
container the GPU and its driver:

```sh
docker run --rm --gpus all -v "$PWD:/work" ghcr.io/sergioffpc/umbra:X.Y.Z \
  render scene.usda --devices=gpu0
```

On a machine with several NUMA nodes, each CPU device keeps its memory on its
node by binding the memory policy of its threads, which Docker's default
seccomp profile allows only with `--cap-add SYS_NICE`. Without it, umbra
warns once per device and renders with its memory where the kernel places it.

### With Podman

On rootless Podman, the image's user, UID 65532, is one of your subordinate
UIDs on the host, so it cannot write to a directory of yours mounted on
`/work`. `--userns=keep-id:uid=65532,gid=65532` maps your own user to it
instead, and umbra's outputs belong to you:

```sh
podman run --rm --userns=keep-id:uid=65532,gid=65532 -v "$PWD:/work:z" \
  ghcr.io/sergioffpc/umbra:X.Y.Z render scene.usda
```

On SELinux hosts, `:z` relabels the mounted directory so the container can
read it: run the command from the directory of your scenes, never from your
home directory. With the NVIDIA Container Toolkit, give the container the
GPUs with `--device nvidia.com/gpu=all`. To list the published tags (Podman
shows at most 25 unless `--limit` says otherwise):

```sh
podman search --list-tags --limit 100 ghcr.io/sergioffpc/umbra
```

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

## Usage

```sh
umbra devices                              # the devices umbra can render on
umbra render scene.usda                    # render on every device
umbra render scene.usda --devices=cpu0,gpu0
umbra render scene.usda --frames=1..24     # a range of frames
umbra render scene.usda --display=tev      # watch the frames in a running tev
umbra version --build-options
```

A render writes its render products into the directory it runs in, and
reports its progress, warnings and errors on stderr; `-q` shows errors only,
and `UMBRA_LOG` adds debug or trace messages. `umbra help` lists the
commands, and `umbra help <command>` every option of one.

The repository's reference scene, the Cornell box, renders a 512×512 image
with its color, albedo, normal, depth and denoised color:

```sh
umbra render tests/scenes/cornell_box.usda
```

## Preparing a scene

Umbra renders a stage through its `UsdRender` settings:

- **Render settings**: the prim given with `--settings`, or else the one the
  stage's `renderSettingsPrimPath` names. They must name a camera.
  `disableMotionBlur` and `disableDepthOfField` are honoured.
- **Render products**: each is written as the OpenEXR file its
  `productName` names (`.exr` only), relative to the directory umbra runs
  in, its directories created. `$F`, `$F4` and `$FF` in the name expand to
  the frame, whole, zero-padded to 4 digits, or exact. Settings with no
  product render one, `<scene>.exr`, with its color.
- **Render vars**: the raw sources `color`, `albedo`, `normal`, `depth` and
  `denoisedColor`, the source name defaulting to the var's name. Channels are
  half, or float when the var's `dataType` is a float type. `lpe` and
  `primvar` vars are written as 0, with a warning, and other vars are left
  out, with a warning.
- **`umbra:samplesPerPixel`**: the samples per pixel, on the render product
  or else the render settings; 64 by default.
- **`umbra:dicingRate`**: on the render settings, the longest a triangle's
  edge of a subdivision surface may look from the camera, in pixels; 1 by
  default.

What umbra renders:

- **Meshes**, subdivision surfaces included (Catmull-Clark, Loop and
  bilinear), with their holes and numeric primvars, skinned by UsdSkel, and
  instanced natively or by point instancers. Implicit surfaces (sphere,
  cube, cylinder, cone, capsule, plane) render as meshes. Curves, points and
  volumes are left out, with a warning.
- **Lights**: UsdLux sphere, rect, disk, cylinder, distant and dome lights,
  dome portals, emissive meshes (MeshLightAPI), light textures, color
  temperature and IES profiles with their TILT data. Light filters, light
  linking, shadow controls, `diffuse` and `specular`, `shaping:focus`,
  `shaping:cone:softness` and `shaping:ies:angleScale` are ignored, with a
  warning.
- **Materials**: MaterialX and UsdPreviewSurface networks, and OSL shaders
  (`.oso`), with their textures read in their color space. A mesh with no
  material renders with its `displayColor`, or as 18% gray; so does a
  material that umbra cannot translate, with a warning.
- **Cameras**: perspective and orthographic, with their lens (focal length,
  aperture, f-stop and focus distance), physical exposure and shutter; prims
  that move within the shutter are sampled over it.
- **Frames**: `--frames` and `--step` choose the time codes; by default, the
  stage's start to end time codes.

## Contributing

To develop umbra, read [CONTRIBUTING.md](CONTRIBUTING.md), and follow the
[Code of Conduct](CODE_OF_CONDUCT.md). Report security vulnerabilities as
described in [SECURITY.md](SECURITY.md). Notable changes are listed in
[CHANGELOG.md](CHANGELOG.md).

## License

Copyright The Umbra Authors. Licensed under the
[Apache License, Version 2.0](LICENSE). See [NOTICE](NOTICE) and
[AUTHORS](AUTHORS).
