# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for OpenColorIO (cmake -C): the shared library alone, without
# the command-line apps, the plugins, the bindings, the tests or the
# documentation. The common settings of the layer come from layers/deps.cmake.
#
# OpenColorIO finds every dependency where the layer puts it, and downloads
# none: yaml-cpp, pystring and Imath are dependencies of the layer, expat,
# zlib and minizip-ng are Ubuntu packages (see third_party/versions.md).
#
# Its CPU processors run on umbra's CPU; its GPU path generates shader code
# for the transforms, which MaterialX's OSL generator asks for in OSL (see
# config/MaterialX.cmake). The API reports errors with exceptions.

set(OCIO_INSTALL_EXT_PACKAGES NONE CACHE STRING "")

set(OCIO_BUILD_APPS OFF CACHE BOOL "")
set(OCIO_BUILD_OPENFX OFF CACHE BOOL "")
set(OCIO_BUILD_NUKE OFF CACHE BOOL "")
set(OCIO_BUILD_PYTHON OFF CACHE BOOL "")
set(OCIO_BUILD_JAVA OFF CACHE BOOL "")
set(OCIO_BUILD_DOCS OFF CACHE BOOL "")
set(OCIO_BUILD_TESTS OFF CACHE BOOL "")
set(OCIO_BUILD_GPU_TESTS OFF CACHE BOOL "")
set(OCIO_VULKAN_ENABLED OFF CACHE BOOL "")

# SIMD: the CPU processors compile their SSE, AVX, AVX2, AVX-512 and F16C
# kernels into the library and pick the best one the CPU supports at run time.
set(OCIO_USE_SIMD ON CACHE BOOL "")
