# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Helpers for the dependency build.
#
#   umbra_add_dependency(<name>
#     [SOURCE_SUBDIR <dir>]            CMake project directory inside the submodule
#     [DEPENDS <dependency>...]        other dependencies of the same layer
#     [CMAKE_ARGS <arg>...]            extra configure arguments
#     [BUILD_TARGETS <target>...]      default: the project's default target
#     [INSTALL_TARGETS <target>...]    default: cmake --install
#     [BUILD_ENVIRONMENT <VAR=value>...])
#
# Adds one ExternalProject for the git submodule third_party/<name>. Patches in
# third_party/patches/<name>/*.patch are applied in name order at build time;
# the submodule is never modified in the repository. Dependencies of a layer
# are built with the compiler the dependency build is configured with, in
# Release, and installed to CMAKE_INSTALL_PREFIX (under UMBRA_STAGING_DIR when
# set).
#
#   umbra_add_layer(<layer> DEPENDENCIES <dependency>...)
#
# Defines the aggregate target layer-<layer> that builds and installs the
# given dependencies. Dependencies are excluded from the default target.

include_guard(GLOBAL)
include(ExternalProject)

set(UMBRA_THIRD_PARTY_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

function(umbra_add_dependency name)
  cmake_parse_arguments(PARSE_ARGV 1 arg ""
                        "SOURCE_SUBDIR"
                        "DEPENDS;CMAKE_ARGS;BUILD_TARGETS;INSTALL_TARGETS;BUILD_ENVIRONMENT")

  set(source_dir "${UMBRA_THIRD_PARTY_DIR}/${name}")
  cmake_path(NORMAL_PATH source_dir)

  # Build trees live in UMBRA_BUILD_CACHE_DIR when given so that an interrupted
  # build resumes there. Stamps stay in this build tree: every fresh configure
  # re-runs patch, configure, build and install, which are incremental.
  if(UMBRA_BUILD_CACHE_DIR)
    set(binary_dir "${UMBRA_BUILD_CACHE_DIR}/${name}")
  else()
    set(binary_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}")
  endif()

  set(build_env ${CMAKE_COMMAND} -E env ${arg_BUILD_ENVIRONMENT})
  set(install_env ${build_env})
  if(UMBRA_STAGING_DIR)
    list(APPEND install_env "DESTDIR=${UMBRA_STAGING_DIR}")
  endif()

  set(build_command ${build_env} ${CMAKE_COMMAND} --build <BINARY_DIR>)
  if(arg_BUILD_TARGETS)
    list(APPEND build_command --target ${arg_BUILD_TARGETS})
  endif()
  if(arg_INSTALL_TARGETS)
    set(install_command ${install_env} ${CMAKE_COMMAND} --build <BINARY_DIR>
                        --target ${arg_INSTALL_TARGETS})
  else()
    set(install_command ${install_env} ${CMAKE_COMMAND} --install <BINARY_DIR>)
  endif()

  ExternalProject_Add(${name}
    SOURCE_DIR "${source_dir}"
    SOURCE_SUBDIR "${arg_SOURCE_SUBDIR}"
    BINARY_DIR "${binary_dir}"
    STAMP_DIR "${CMAKE_CURRENT_BINARY_DIR}/stamps/${name}"
    TMP_DIR "${CMAKE_CURRENT_BINARY_DIR}/tmp/${name}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    PATCH_COMMAND ${CMAKE_COMMAND}
                  "-DSOURCE_DIR=${source_dir}"
                  "-DPATCH_DIR=${UMBRA_THIRD_PARTY_DIR}/patches/${name}"
                  -P "${UMBRA_THIRD_PARTY_DIR}/cmake/ApplyPatches.cmake"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_ARGS
      "-DCMAKE_BUILD_TYPE=Release"
      "-DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}"
      "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DCMAKE_AR=${CMAKE_AR}"
      "-DCMAKE_RANLIB=${CMAKE_RANLIB}"
      ${arg_CMAKE_ARGS}
    BUILD_COMMAND ${build_command}
    INSTALL_COMMAND ${install_command}
    DEPENDS ${arg_DEPENDS}
    LIST_SEPARATOR |
    EXCLUDE_FROM_ALL TRUE
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE
    USES_TERMINAL_INSTALL TRUE)
endfunction()

function(umbra_add_layer layer)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "DEPENDENCIES")
  add_custom_target(layer-${layer})
  if(arg_DEPENDENCIES)
    add_dependencies(layer-${layer} ${arg_DEPENDENCIES})
  endif()
endfunction()
