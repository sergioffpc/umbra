# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
#   umbra_add_build_info(<target> TEMPLATE <file.in>)
#
# Generates a source file from TEMPLATE on every build and adds it to
# <target>. The template sees these variables (@VAR@ syntax):
#
#   UMBRA_COMMIT      full hash of HEAD, with "-dirty" appended when tracked
#                     files have uncommitted changes; "unknown" outside git
#   UMBRA_BUILD_TYPE  the build type (CMAKE_BUILD_TYPE)
#   UMBRA_ARCH        the target architecture
#   UMBRA_COMPILER    the compiler name and version, e.g. "clang 23.1.6"
#
# The file is rewritten only when its contents change, so a new commit (or a
# change in the dirty state) recompiles that one file and relinks; nothing
# else rebuilds.

include_guard(GLOBAL)

set(UMBRA_BUILD_INFO_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/GenerateBuildInfo.cmake")

function(umbra_add_build_info target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "TEMPLATE" "")
  cmake_path(ABSOLUTE_PATH arg_TEMPLATE
             BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  cmake_path(GET arg_TEMPLATE STEM LAST_ONLY output_name)
  set(output "${CMAKE_CURRENT_BINARY_DIR}/${output_name}")
  string(TOLOWER "${CMAKE_CXX_COMPILER_ID}" compiler)

  find_package(Git QUIET)
  add_custom_target(${target}_build_info
    COMMAND "${CMAKE_COMMAND}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
            "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DTEMPLATE=${arg_TEMPLATE}"
            "-DOUTPUT=${output}"
            "-DUMBRA_BUILD_TYPE=$<CONFIG>"
            "-DUMBRA_ARCH=${UMBRA_ARCH}"
            "-DUMBRA_COMPILER=${compiler} ${CMAKE_CXX_COMPILER_VERSION}"
            -P "${UMBRA_BUILD_INFO_SCRIPT}"
    BYPRODUCTS "${output}"
    COMMENT "Generating build info"
    VERBATIM)
  target_sources(${target} PRIVATE "${output}")
  add_dependencies(${target} ${target}_build_info)
endfunction()
