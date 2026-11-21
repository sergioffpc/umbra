# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# umbra-deps: the toolchain image plus the third-party libraries built by the
# superbuild (layer "deps"), the sources they were built from and a static
# clangd index of those sources. Everything lives under /opt/umbra:
#
#   include/, lib/, lib/cmake/  the libraries; CMAKE_PREFIX_PATH points here
#   src/                        the superbuild and, in src/<dependency>/,
#                               the (patched) sources, kept for navigation
#   share/clangd/deps.idx       clangd index of src/, with absolute paths
#                               under /opt/umbra/src (for an External index)
#
# Build from the repository root with the layer's submodules checked out, on
# top of the toolchain image whose tag the deps layer hash includes:
#   docker buildx build -f docker/deps.Dockerfile \
#     --build-arg TOOLCHAIN_IMAGE=ghcr.io/<owner>/umbra-toolchain:<tag> .

ARG TOOLCHAIN_IMAGE

# The superbuild runs from its copy under the prefix, so the compile databases
# and the clangd index point at the sources that stay in the image.
FROM ${TOOLCHAIN_IMAGE} AS build
COPY third_party/ /opt/umbra/src/
RUN cmake -S /opt/umbra/src -B /tmp/superbuild -G Ninja \
          -DUMBRA_LAYER=deps \
          -DCMAKE_INSTALL_PREFIX=/opt/umbra \
 && cmake --build /tmp/superbuild --target layer-deps

FROM ${TOOLCHAIN_IMAGE} AS deps
COPY --from=build /opt/umbra/ /opt/umbra/
ENV CMAKE_PREFIX_PATH=/opt/umbra
