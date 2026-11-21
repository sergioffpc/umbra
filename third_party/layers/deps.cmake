# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Layer "deps": the libraries umbra builds against (GoogleTest and Google
# Benchmark so far), built with the toolchain image's clang. Every dependency
# is a Release shared library without LTO, against libc++, for x86-64-v3,
# without exceptions where the project allows it, and finds the libraries next
# to it through RUNPATH ($ORIGIN).
#
# Every dependency also exports its compile database. The last step of the
# layer runs clangd-indexer over all of them and writes one static clangd
# index to <prefix>/share/clangd/deps.idx. The index records the absolute
# paths the sources were compiled from, so configure the superbuild from a
# copy of third_party/ that stays in the image, under the install prefix
# (docker/deps.Dockerfile uses <prefix>/src):
#
#   cmake -S <prefix>/src -B <build> -G Ninja -DUMBRA_LAYER=deps \
#         -DCMAKE_INSTALL_PREFIX=<prefix>
#   cmake --build <build> --target layer-deps

set(umbra_deps_arch x86-64-v3)

find_program(UMBRA_CLANGD_INDEXER clangd-indexer REQUIRED)

# Settings shared by every dependency of the layer; config/<dependency>.cmake
# adds the project's own options.
set(umbra_deps_cmake_args
  -DBUILD_SHARED_LIBS=ON
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF
  "-DCMAKE_C_FLAGS=-march=${umbra_deps_arch}"
  "-DCMAKE_CXX_FLAGS=-march=${umbra_deps_arch} -stdlib=libc++"
  -DCMAKE_SHARED_LINKER_FLAGS=-Wl,--enable-new-dtags
  -DCMAKE_EXE_LINKER_FLAGS=-Wl,--enable-new-dtags
  -DCMAKE_INSTALL_LIBDIR=lib
  "-DCMAKE_INSTALL_RPATH=$ORIGIN"
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)

set(umbra_deps googletest benchmark)
foreach(dependency IN LISTS umbra_deps)
  umbra_add_dependency(${dependency}
    CMAKE_ARGS
      ${umbra_deps_cmake_args}
      -C "${CMAKE_CURRENT_LIST_DIR}/../config/${dependency}.cmake")
endforeach()

# The static clangd index over all dependency sources. It is rebuilt whenever
# the layer is.
set(umbra_deps_index "${UMBRA_STAGING_DIR}${CMAKE_INSTALL_PREFIX}/share/clangd/deps.idx")
set(umbra_deps_databases "")
foreach(dependency IN LISTS umbra_deps)
  ExternalProject_Get_Property(${dependency} BINARY_DIR)
  list(APPEND umbra_deps_databases "${BINARY_DIR}/compile_commands.json")
endforeach()
list(JOIN umbra_deps_databases "|" umbra_deps_databases)
add_custom_target(deps-clangd-index
  COMMAND ${CMAKE_COMMAND}
          "-DINDEXER=${UMBRA_CLANGD_INDEXER}"
          "-DDATABASES=${umbra_deps_databases}"
          "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/clangd-index"
          "-DOUTPUT=${umbra_deps_index}"
          -P "${UMBRA_SUPERBUILD_DIR}/cmake/BuildClangdIndex.cmake"
  COMMENT "Indexing the dependency sources for clangd"
  USES_TERMINAL
  VERBATIM)
add_dependencies(deps-clangd-index ${umbra_deps})

umbra_add_layer(deps DEPENDENCIES ${umbra_deps} deps-clangd-index)
