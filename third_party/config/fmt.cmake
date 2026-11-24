# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for {fmt} (cmake -C): the shared libfmt and the header-only
# target, both in the fmt CMake package, without the docs, the tests or the
# fuzzers. The common settings of the layer come from layers/deps.cmake.
#
# OpenImageIO formats its messages with it. It reports format errors with
# exceptions, unless the format string is checked at compile time.

set(FMT_INSTALL ON CACHE BOOL "")
set(FMT_DOC OFF CACHE BOOL "")
set(FMT_TEST OFF CACHE BOOL "")
set(FMT_FUZZ OFF CACHE BOOL "")
set(FMT_CUDA_TEST OFF CACHE BOOL "")
# The headers, not a C++20 module: OpenImageIO and umbra include them.
set(FMT_MODULE OFF CACHE BOOL "")
set(FMT_OS ON CACHE BOOL "")
set(FMT_UNICODE ON CACHE BOOL "")
