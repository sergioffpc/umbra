# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for GoogleTest (cmake -C): GoogleTest and GoogleMock only, no
# Abseil. The common settings of the layer come from layers/deps.cmake.
#
# GoogleTest has no option to build without exceptions: it compiles its own
# libraries with -fexceptions whatever the flags. Code built with
# -fno-exceptions still uses them, with GTEST_HAS_EXCEPTIONS detected as 0.

set(BUILD_GMOCK ON CACHE BOOL "")
set(INSTALL_GTEST ON CACHE BOOL "")
set(GTEST_HAS_ABSL OFF CACHE BOOL "")
set(gtest_build_tests OFF CACHE BOOL "")
set(gtest_build_samples OFF CACHE BOOL "")
set(gmock_build_tests OFF CACHE BOOL "")
