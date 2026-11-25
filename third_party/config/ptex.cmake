# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for Ptex (cmake -C): the shared library, its CMake package and
# ptxinfo, which prints the metadata and the faces of a .ptx file, without the
# static library or the docs. Ptex always builds its tests, which it does not
# install. The common settings of the layer come from layers/deps.cmake.
#
# OpenImageIO reads .ptx files through Ptex, in its embedded Ptex plugin.
# Ptex compresses with libdeflate, from Ubuntu, through its CMake package.

set(PTEX_BUILD_SHARED_LIBS ON CACHE BOOL "")
set(PTEX_BUILD_STATIC_LIBS OFF CACHE BOOL "")
set(PTEX_BUILD_DOCS OFF CACHE BOOL "")

# ptxinfo is installed to bin/ and finds the library in lib/; for the library
# itself the second entry is its own directory again.
set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib" CACHE STRING "" FORCE)

# Ptex takes its version from git describe, and the layer builds from a copy
# of the sources without their git history, where it would fall back to
# v2.4.X: the library's soname, PtexVersion.h and the version OpenImageIO
# checks come from these. Keep them equal to the tag and commit of the Ptex
# row in versions.md.
set(PTEX_VER "v2.5.4" CACHE STRING "")
set(PTEX_SHA "42787491dbea182e339074d305b782b14f1ca2be" CACHE STRING "")
