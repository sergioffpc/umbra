# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Applies PATCH_DIR/*.patch to SOURCE_DIR in name order. Idempotent: a patch
# that is already applied is skipped, so a rebuild over the same source tree
# is safe. Run as: cmake -DSOURCE_DIR=<dir> -DPATCH_DIR=<dir> -P ApplyPatches.cmake

cmake_minimum_required(VERSION 4.2)

if(NOT IS_DIRECTORY "${SOURCE_DIR}")
  message(FATAL_ERROR "ApplyPatches: SOURCE_DIR '${SOURCE_DIR}' is not a directory")
endif()

file(GLOB patches LIST_DIRECTORIES false "${PATCH_DIR}/*.patch")
list(SORT patches)
if(NOT patches)
  return()
endif()

find_program(GIT_EXECUTABLE git REQUIRED)

foreach(patch IN LISTS patches)
  cmake_path(GET patch FILENAME patch_name)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --check --reverse "${patch}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE already_applied
    OUTPUT_QUIET ERROR_QUIET)
  if(already_applied EQUAL 0)
    message(STATUS "ApplyPatches: ${patch_name} already applied")
    continue()
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${patch}"
    WORKING_DIRECTORY "${SOURCE_DIR}"
    COMMAND_ERROR_IS_FATAL ANY)
  message(STATUS "ApplyPatches: applied ${patch_name}")
endforeach()
