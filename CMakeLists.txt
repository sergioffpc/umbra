# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Umbra. Configure through the presets in CMakePresets.json (or the Makefile),
# inside the devcontainer: the toolchain is LLVM with libc++, compiler-rt,
# libunwind and lld, and the dependencies are found in CMAKE_PREFIX_PATH.

cmake_minimum_required(VERSION 4.2)

project(umbra VERSION 0.1.0
        DESCRIPTION "Umbra renderer"
        HOMEPAGE_URL "https://github.com/sergioffpc/umbra"
        LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(UmbraCompiler)
include(UmbraWarnings)
include(UmbraSafety)
include(UmbraSanitizers)
include(UmbraHardening)
include(UmbraModules)

enable_testing()

add_subdirectory(src/core)
add_subdirectory(tests)
