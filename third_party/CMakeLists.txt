# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Third-party superbuild.
#
# Builds the git submodules under third_party/ with one ExternalProject each.
# Every container image layer is one file in layers/ that defines its
# dependencies and an aggregate target named layer-<name>. A configure selects
# exactly one layer, so a layer's image depends only on its own files:
#
#   cmake -S third_party -B <build> -G Ninja -DUMBRA_LAYER=toolchain
#   cmake --build <build> --target layer-toolchain
#
# The compiler this project is configured with is the compiler every
# dependency of the selected layer is built with.

cmake_minimum_required(VERSION 4.2)

project(umbra_third_party LANGUAGES C CXX)

set(UMBRA_LAYER "" CACHE STRING "Image layer to build (a file name in layers/, without extension)")
set(UMBRA_STAGING_DIR "" CACHE PATH
    "Optional DESTDIR for the install step; files land in <dir>/<CMAKE_INSTALL_PREFIX>")
set(UMBRA_BUILD_CACHE_DIR "" CACHE PATH
    "Optional directory for dependency build trees, kept across builds (e.g. a CI cache)")

set(umbra_layer_file "${CMAKE_CURRENT_SOURCE_DIR}/layers/${UMBRA_LAYER}.cmake")
if(UMBRA_LAYER STREQUAL "" OR NOT EXISTS "${umbra_layer_file}")
  file(GLOB umbra_known_layers RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/layers"
       "${CMAKE_CURRENT_SOURCE_DIR}/layers/*.cmake")
  list(TRANSFORM umbra_known_layers REPLACE "\\.cmake$" "")
  message(FATAL_ERROR
          "UMBRA_LAYER must name one of: ${umbra_known_layers} (got '${UMBRA_LAYER}')")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(UmbraSuperbuild)

include("${umbra_layer_file}")

if(NOT TARGET layer-${UMBRA_LAYER})
  message(FATAL_ERROR "${umbra_layer_file} must define the target layer-${UMBRA_LAYER}")
endif()
