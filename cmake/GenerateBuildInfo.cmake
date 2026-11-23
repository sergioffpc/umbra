# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Script run at build time by umbra_add_build_info (UmbraBuildInfo.cmake):
#
#   cmake -DGIT_EXECUTABLE=<git> -DSOURCE_DIR=<dir> -DTEMPLATE=<file.in>
#         -DOUTPUT=<file> -DUMBRA_BUILD_TYPE=<type> -DUMBRA_ARCH=<arch>
#         -DUMBRA_COMPILER=<compiler> -P GenerateBuildInfo.cmake
#
# configure_file() leaves OUTPUT untouched when its contents do not change.

cmake_minimum_required(VERSION 4.2)

set(UMBRA_COMMIT "unknown")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE head
    RESULT_VARIABLE head_status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(head_status EQUAL 0)
    set(UMBRA_COMMIT "${head}")
    # Untracked files do not make the tree dirty, as in git describe --dirty.
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
      WORKING_DIRECTORY "${SOURCE_DIR}"
      OUTPUT_VARIABLE changes
      RESULT_VARIABLE status_status
      ERROR_QUIET)
    if(NOT status_status EQUAL 0 OR NOT changes STREQUAL "")
      string(APPEND UMBRA_COMMIT "-dirty")
    endif()
  endif()
endif()

configure_file("${TEMPLATE}" "${OUTPUT}" @ONLY)
