# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# The release image of umbra (docs/adr/0005-releases-are-a-chiseled-image.md):
# umbra's installed tree under /opt/umbra on the Ubuntu libraries it loads,
# cut with Chisel from the Ubuntu release of the deps image, with no shell
# and no package manager:
#
#   /opt/umbra/bin/umbra          the entrypoint
#   /opt/umbra/lib/               the GPU module, the libraries umbra builds
#                                 or takes from the toolchain, and usd/
#   /opt/umbra/plugin/usd/        OpenUSD's other plugins
#   /opt/umbra/share/licenses/    the license texts of umbra and of each
#                                 component under /opt/umbra
#   /usr/lib/<triplet>/           the Ubuntu libraries, with their copyright
#                                 files in /usr/share/doc/<package>/
#   /var/lib/chisel/              Chisel's manifest of the slices and files
#   /work                         the working directory, for scenes and
#                                 outputs
#
# It runs as umbra, UID and GID 65532, which /etc/passwd and /etc/group name.
#
# Build from the repository root on the deps image the devcontainer pins
# (no submodules needed: the deps image carries the dependencies):
#   docker buildx build -f docker/release.Dockerfile \
#     --build-arg DEPS_IMAGE=ghcr.io/<owner>/umbra-deps:<tag> .

ARG DEPS_IMAGE

# umbra's release preset, installed into /out, the tree of /opt/umbra: the
# binaries find their libraries through RUNPATH=$ORIGIN/../lib, so the tree
# works from any prefix. Only umbra and the GPU module are built: the
# install takes nothing else. The build tree lives in the source mount and is
# discarded with it.
FROM ${DEPS_IMAGE} AS build
SHELL ["/bin/bash", "-o", "errexit", "-o", "nounset", "-o", "pipefail", "-c"]
WORKDIR /src
RUN --mount=type=bind,target=/src,rw \
  cmake --preset release \
  && cmake --build --preset release --target umbra umbra_gpu_backend \
  && cmake --install build/release --prefix /out

# The Ubuntu libraries of the tree and their copyright files, cut by Chisel
# into /rootfs. The release of the slices, canonical/chisel-releases at a
# pinned commit of its branch for the deps image's Ubuntu release, gets our
# slices of docker/chisel/slices/ for the packages it does not slice yet. Its
# chisel.yaml must name the deps image's release, so the libraries are those
# the tree was built against. Both downloads are verified by SHA-256 (the
# release of Chisel publishes a SHA-384 only).
FROM ${DEPS_IMAGE} AS chisel
ARG CHISEL_VERSION=1.5.1
ARG CHISEL_SHA256=caa1f84ef144c3311d95ee464c1b72dd237e99055a8c58b791a5591abd42d9a1
ARG CHISEL_RELEASES_COMMIT=660fe6b93063ce85286fb51f759cc55ddf24d9a1
ARG CHISEL_RELEASES_SHA256=83e24785e902c14fed2aac3b7829240a964269806ff608eac401816bc86913aa
SHELL ["/bin/bash", "-o", "errexit", "-o", "nounset", "-o", "pipefail", "-c"]
WORKDIR /tmp/chisel
RUN readonly GITHUB=https://github.com/canonical; \
  readonly RELEASE="${GITHUB}/chisel/releases/download/v${CHISEL_VERSION}"; \
  curl -fsSLo chisel.tar.gz \
    "${RELEASE}/chisel_v${CHISEL_VERSION}_linux_amd64.tar.gz"; \
  echo "${CHISEL_SHA256}  chisel.tar.gz" | sha256sum -c -; \
  curl -fsSLo chisel-releases.tar.gz \
    "${GITHUB}/chisel-releases/archive/${CHISEL_RELEASES_COMMIT}.tar.gz"; \
  echo "${CHISEL_RELEASES_SHA256}  chisel-releases.tar.gz" | sha256sum -c -; \
  tar -xzf chisel.tar.gz chisel; \
  install -d /chisel-releases; \
  tar -xzf chisel-releases.tar.gz -C /chisel-releases --strip-components=1
# The slices: base-files gives the directory tree, /etc/os-release and the
# manifest, libc6 glibc; libedit2, libxml2-16, zlib1g and libzstd1 are what
# libLLVM links; the others are the C libraries of the dependencies (see
# third_party/versions.md) and those they link. Each slice brings the
# copyright slice of its package. The user and group files are ours, without
# a shell for either.
RUN --mount=type=bind,source=docker/chisel/slices,target=/tmp/chisel/slices \
  source /etc/os-release; \
  if ! grep -Eq "^[[:space:]]+version: ${VERSION_ID}\$" \
    /chisel-releases/chisel.yaml; then \
    echo "the chisel release is not for Ubuntu ${VERSION_ID}" >&2; \
    exit 1; \
  fi; \
  for slice in slices/*.yaml; do \
    name="${slice##*/}"; \
    if [[ -e "/chisel-releases/slices/${name}" ]]; then \
      echo "canonical/chisel-releases slices ${name%.yaml} now:" \
        "delete docker/chisel/slices/${name}" >&2; \
      exit 1; \
    fi; \
    cp "${slice}" /chisel-releases/slices/; \
  done; \
  install -d /rootfs; \
  ./chisel cut --release /chisel-releases --root /rootfs --arch amd64 \
    base-files_base base-files_release-info base-files_chisel \
    libc6_libs \
    libbrotli1_libs libbsd0_libs libbz2-1.0_libs libdeflate0_libs \
    libedit2_libs libexpat1_libs libfreetype6_libs libgif7_libs \
    libjpeg-turbo8_libs libmd0_libs libminizip-ng4t64_libs \
    libopenjp2-7_libs libpng16-16t64_libs libsharpyuv0_libs \
    libtinfo6_libs libwebp7_libs libwebpdemux2_libs libwebpmux3_libs \
    libxml2-16_libs libzstd1_libs zlib1g_libs; \
  printf '%s\n' 'root:x:0:0:root:/root:/usr/sbin/nologin' \
    'umbra:x:65532:65532:umbra:/home/umbra:/usr/sbin/nologin' \
    >/rootfs/etc/passwd; \
  printf '%s\n' 'root:x:0:' 'umbra:x:65532:' >/rootfs/etc/group; \
  install -d -o 65532 -g 65532 -m 0755 /rootfs/home/umbra /rootfs/work

FROM scratch AS release
COPY --from=chisel /rootfs/ /
COPY --from=build /out/ /opt/umbra/
USER 65532:65532
WORKDIR /work
ENTRYPOINT ["/opt/umbra/bin/umbra"]
CMD ["--help"]
