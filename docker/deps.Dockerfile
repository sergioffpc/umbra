# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# umbra-deps: the toolchain image plus the third-party libraries built by the
# dependency build (layer "deps"), the sources they were built from and a static
# clangd index of those sources. Everything lives under /opt/umbra:
#
#   include/, lib/, lib/cmake/  the libraries; CMAKE_PREFIX_PATH points here
#   tsan/                       oneTBB instrumented with ThreadSanitizer, for
#                               the tsan preset
#   src/                        the dependency build and, in src/<dependency>/,
#                               the (patched) sources, kept for navigation and
#                               for the license texts umbra's install ships
#   share/clangd/deps.idx       clangd index of src/, with absolute paths
#                               under /opt/umbra/src (for an External index)
#
# The dependencies with GPU backends build against the NVIDIA development
# stack the toolchain image carries under /opt/cuda, and Open Image Denoise's
# CPU device with its ISPC. Like the toolchain image, this image is a private
# package.
#
# The image also carries tev, the HDR image viewer, in /opt/tev, on PATH: a
# tool umbra does not use, taken prebuilt from its release
# (docs/adr/0018-prebuilt-tools-of-the-deps-image.md).
#
# Build from the repository root with the layer's submodules checked out, on
# top of the toolchain image whose tag the deps layer hash includes:
#   docker buildx build -f docker/deps.Dockerfile \
#     --build-arg TOOLCHAIN_IMAGE=ghcr.io/<owner>/umbra-toolchain:<tag> .

ARG TOOLCHAIN_IMAGE

# The dependency build runs from its copy under the prefix, so the compile
# databases and the clangd index point at the sources that stay in the image.
# The dependencies of the dependencies are Ubuntu packages, not submodules:
# their -dev packages are installed here, their runtime packages in the final
# stage (see third_party/versions.md).
#
# Every compiler runs through ccache, whose cache is the BuildKit cache mount
# below: the deps workflow loads it before the build and saves it after, so a
# build recompiles only the files whose source, flags or compiler changed.
# The paths of the build are fixed, and ccache hashes the compiler's content,
# so a hit is a file compiled the same way. ccache stays in this stage only.
FROM ${TOOLCHAIN_IMAGE} AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    libhwloc-dev libdeflate-dev libzstd-dev ccache \
    libexpat1-dev zlib1g-dev libminizip-ng-dev \
    libjpeg-turbo8-dev libpng-dev libwebp-dev libgif-dev libopenjp2-7-dev \
    libfreetype-dev flex bison \
  && rm -rf /var/lib/apt/lists/*
ENV CCACHE_DIR=/var/cache/ccache \
    CCACHE_MAXSIZE=5G \
    CCACHE_COMPILERCHECK=content
COPY third_party/ /opt/umbra/src/
RUN --mount=type=cache,id=umbra-deps-ccache,target=/var/cache/ccache,sharing=locked \
  ccache --zero-stats \
  && cmake -S /opt/umbra/src -B /tmp/dependency-build -G Ninja \
    -DUMBRA_LAYER=deps \
    -DCMAKE_INSTALL_PREFIX=/opt/umbra \
  && cmake --build /tmp/dependency-build --target layer-deps \
  && ccache --show-stats

# tev's Linux release is an AppImage, unpacked here without FUSE. Its
# executable finds the libraries the AppImage bundles in ../lib; the window
# libraries come from Ubuntu, in the final stage.
FROM ${TOOLCHAIN_IMAGE} AS tev
ARG DEBIAN_FRONTEND=noninteractive
ARG TEV_VERSION=2.14.0
ARG TEV_SHA256=a7d4147c2984b93c135ac2559422cb1f80310cf971b07056320e8fbbd63fdf82
SHELL ["/bin/bash", "-o", "errexit", "-o", "nounset", "-o", "pipefail", "-c"]
RUN apt-get update \
  && apt-get install -y --no-install-recommends ca-certificates curl \
  && rm -rf /var/lib/apt/lists/*
RUN curl -fsSLo /tmp/tev.appimage \
    "https://github.com/Tom94/tev/releases/download/v${TEV_VERSION}/tev.appimage"; \
  echo "${TEV_SHA256}  /tmp/tev.appimage" | sha256sum -c -; \
  chmod +x /tmp/tev.appimage; \
  cd /tmp; \
  ./tev.appimage --appimage-extract >/dev/null; \
  install -d /opt/tev; \
  cp -a /tmp/squashfs-root/usr/. /opt/tev/; \
  rm -rf /tmp/tev.appimage /tmp/squashfs-root

FROM ${TOOLCHAIN_IMAGE} AS deps
ARG DEBIAN_FRONTEND=noninteractive
# hwloc: oneTBB's libtbbbind, for NUMA-aware task arenas. libdeflate and
# zstd: OpenEXR's ZIP and ZSTD compression, libdeflate also Ptex's. expat,
# zlib and minizip-ng: OpenColorIO's CLF/CTF files and .ocioz archives.
# libjpeg-turbo, libpng, libwebp, giflib, OpenJPEG and FreeType: OpenImageIO's
# JPEG, PNG, WebP, GIF and JPEG 2000 formats and its text rendering; libjpeg,
# libwebp and zstd also libtiff's codecs. zlib and zstd: OpenVDB's grid
# compression; their -dev packages too, since FindOpenVDB.cmake, through
# which umbra finds OpenVDB, finds them for its own targets. The X11,
# Wayland, xkbcommon and OpenGL libraries: tev's window, most of which its
# GLFW loads at run time, and its clipboard; Mesa draws it where the NVIDIA
# driver's OpenGL is not mounted.
RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    libhwloc15 libdeflate0 libzstd1 libexpat1 zlib1g libminizip-ng4t64 \
    zlib1g-dev libzstd-dev \
    libjpeg-turbo8 libpng16-16t64 libwebp7 libwebpdemux2 libwebpmux3 \
    libgif7 libopenjp2-7 libfreetype6 \
    libx11-6 libx11-xcb1 libxcb1 libxrandr2 libxinerama1 libxcursor1 libxi6 \
    libxext6 libxrender1 libxkbcommon0 libwayland-client0 libwayland-cursor0 \
    libwayland-egl1 libgl1 libegl1 \
    libgl1-mesa-dri libglx-mesa0 libegl-mesa0 \
  && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/umbra/ /opt/umbra/
COPY --from=tev /opt/tev/ /opt/tev/
ENV CMAKE_PREFIX_PATH=/opt/umbra \
    PATH=${PATH}:/opt/tev/bin

# The devcontainer's non-root user. It takes uid/gid 1000 from Ubuntu's
# default user, which the devcontainer maps the host user onto (podman
# keep-id). The directories mounted as named volumes exist here, owned by dev,
# so that a new volume starts out owned by dev too. CI keeps running as root.
RUN userdel --remove ubuntu \
  && useradd --create-home --uid 1000 --user-group --shell /bin/bash dev \
  && install -d -o dev -g dev \
    /home/dev/.claude /home/dev/.config /home/dev/.config/gh
