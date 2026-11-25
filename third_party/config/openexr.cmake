# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for OpenEXR (cmake -C): OpenEXRCore (C API), OpenEXR,
# OpenEXRUtil, Iex and IlmThread, and the exr* command line tools, without
# examples, tests, Python bindings or documentation. The common settings of
# the layer come from layers/deps.cmake.
#
# The C++ API (Imf) reports errors by throwing Iex exceptions, so umbra code
# that uses it belongs in src/ext/; OpenEXRCore is a C API without them.

set(OPENEXR_BUILD_LIBS ON CACHE BOOL "")
set(OPENEXR_BUILD_TOOLS ON CACHE BOOL "")
set(OPENEXR_INSTALL_TOOLS ON CACHE BOOL "")

set(OPENEXR_BUILD_EXAMPLES OFF CACHE BOOL "")
set(OPENEXR_BUILD_PYTHON OFF CACHE BOOL "")
set(OPENEXR_BUILD_OSS_FUZZ OFF CACHE BOOL "")
set(OPENEXR_INSTALL_DOCS OFF CACHE BOOL "")
set(BUILD_WEBSITE OFF CACHE BOOL "")
set(BUILD_TESTING OFF CACHE BOOL "")

# The tools are installed to bin/ and find the libraries in lib/; for the
# libraries themselves the second entry is their own directory again.
# patches/openexr keeps this as the whole RUNPATH.
set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib" CACHE STRING "" FORCE)

# The global thread pool of IlmThread runs on the layer's oneTBB, so that
# reading and writing images shares the thread pool of the process. Thread
# pools an application creates itself still use their own threads.
set(OPENEXR_ENABLE_THREADING ON CACHE BOOL "")
set(OPENEXR_USE_TBB ON CACHE BOOL "")

# Imath from the layer. libdeflate and zstd are C libraries and come from the
# Ubuntu packages (see versions.md); layers/deps.cmake fails when they are
# missing, which would otherwise make OpenEXR fall back to its vendored copies
# silently. OpenJPH (HTJ2K) has a C++ API, so the Ubuntu package, built
# against libstdc++, cannot be used: the vendored copy is compiled into
# OpenEXRCore with the layer's toolchain.
set(OPENEXR_FORCE_INTERNAL_IMATH OFF CACHE BOOL "")
set(OPENEXR_FORCE_INTERNAL_DEFLATE OFF CACHE BOOL "")
set(OPENEXR_FORCE_INTERNAL_ZSTD OFF CACHE BOOL "")
set(OPENEXR_FORCE_INTERNAL_OPENJPH ON CACHE BOOL "")
