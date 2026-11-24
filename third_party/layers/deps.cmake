# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Layer "deps": the libraries umbra builds against (GoogleTest, Google
# Benchmark, oneTBB, Embree, OpenQMC, Imath, OpenEXR, OpenSubdiv and
# MaterialX), built with the toolchain image's clang.
# Every dependency is a Release shared library without LTO, against libc++,
# for x86-64-v3, without exceptions where the project allows it, and finds
# the libraries next to it through RUNPATH ($ORIGIN).
#
# Every dependency also exports its compile database. The last step of the
# layer runs clangd-indexer over all of them and writes one static clangd
# index to <prefix>/share/clangd/deps.idx. The index records the absolute
# paths the sources were compiled from, so configure the dependency build from a
# copy of third_party/ that stays in the image, under the install prefix
# (docker/deps.Dockerfile uses <prefix>/src):
#
#   cmake -S <prefix>/src -B <build> -G Ninja -DUMBRA_LAYER=deps \
#         -DCMAKE_INSTALL_PREFIX=<prefix>
#   cmake --build <build> --target layer-deps

set(umbra_deps_arch x86-64-v3)

find_program(UMBRA_CLANGD_INDEXER clangd-indexer REQUIRED)

# hwloc, an Ubuntu package (libhwloc-dev), for oneTBB's libtbbbind.
find_library(UMBRA_HWLOC_LIBRARY hwloc REQUIRED)
find_path(UMBRA_HWLOC_INCLUDE_DIR hwloc.h REQUIRED)
set(umbra_deps_hwloc_args
  "-DCMAKE_HWLOC_2_5_LIBRARY_PATH=${UMBRA_HWLOC_LIBRARY}"
  "-DCMAKE_HWLOC_2_5_INCLUDE_PATH=${UMBRA_HWLOC_INCLUDE_DIR}")

# libdeflate and zstd, Ubuntu packages (libdeflate-dev, libzstd-dev), for
# OpenEXR, which finds them itself but falls back to vendored copies without
# a word when they are missing: requiring them here makes that an error.
find_library(UMBRA_DEFLATE_LIBRARY deflate REQUIRED)
find_path(UMBRA_DEFLATE_INCLUDE_DIR libdeflate.h REQUIRED)
find_library(UMBRA_ZSTD_LIBRARY zstd REQUIRED)
find_path(UMBRA_ZSTD_INCLUDE_DIR zstd.h REQUIRED)

# nvcc, for the dependencies with CUDA code, as cmake/UmbraCuda.cmake sets it
# up for the GPU module: clang and libc++ as the host compiler, which nvcc
# does not support yet, and sm_120 SASS only.
find_program(UMBRA_NVCC nvcc HINTS "$ENV{CUDAToolkit_ROOT}/bin" /opt/cuda/bin
             REQUIRED)
set(umbra_deps_cuda_args
  "-DCMAKE_CUDA_COMPILER=${UMBRA_NVCC}"
  "-DCMAKE_CUDA_HOST_COMPILER=${CMAKE_CXX_COMPILER}"
  "-DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler -Xcompiler=-stdlib=libc++ -Xcompiler=-march=${umbra_deps_arch}"
  -DCMAKE_CUDA_ARCHITECTURES=120-real)

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

# Dependencies that build against another dependency of the layer find it in
# the install prefix (under the staging directory when there is one).
set(umbra_deps googletest benchmark oneTBB embree openqmc Imath openexr OpenSubdiv
  MaterialX)
set(umbra_deps_depends_embree oneTBB)
set(umbra_deps_depends_openexr oneTBB Imath)
set(umbra_deps_depends_OpenSubdiv oneTBB)
set(umbra_deps_args_OpenSubdiv ${umbra_deps_cuda_args})
set(umbra_deps_args_oneTBB ${umbra_deps_hwloc_args})
foreach(dependency IN LISTS umbra_deps)
  umbra_add_dependency(${dependency}
    DEPENDS ${umbra_deps_depends_${dependency}}
    CMAKE_ARGS
      ${umbra_deps_cmake_args}
      "-DCMAKE_PREFIX_PATH=${UMBRA_STAGING_DIR}${CMAKE_INSTALL_PREFIX}"
      ${umbra_deps_args_${dependency}}
      -C "${CMAKE_CURRENT_LIST_DIR}/../config/${dependency}.cmake")
endforeach()

# A second libtbb, instrumented with ThreadSanitizer, for the tsan preset of
# umbra, in its own prefix <prefix>/tsan (include/, lib/, lib/cmake/TBB/).
# ThreadSanitizer cannot see the synchronization of an uninstrumented libtbb,
# and the races it then reports are in oneTBB's inline code, compiled into
# umbra, where no suppression can tell them from umbra's own races. The
# library leaves the ThreadSanitizer runtime to the executable, so only an
# instrumented program can load it. oneTBB's TBB_SANITIZE option is not used:
# it adds GCC's -Wno-tsan, which fails its own flag check under clang.
#
# It is a step of the oneTBB project, a second build tree of the same patched
# source, so that the shared dependency helpers (an input of every layer)
# stay as they are.
ExternalProject_Get_Property(oneTBB BINARY_DIR)
set(umbra_tbb_tsan_dir "${BINARY_DIR}-tsan")
set(umbra_tbb_tsan_install_env "")
if(UMBRA_STAGING_DIR)
  set(umbra_tbb_tsan_install_env "DESTDIR=${UMBRA_STAGING_DIR}")
endif()
ExternalProject_Add_Step(oneTBB tsan
  COMMENT "Building the ThreadSanitizer libtbb"
  COMMAND ${CMAKE_COMMAND} -S <SOURCE_DIR> -B "${umbra_tbb_tsan_dir}"
          -G "${CMAKE_GENERATOR}"
          -DCMAKE_BUILD_TYPE=Release
          "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
          "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
          "-DCMAKE_AR=${CMAKE_AR}"
          "-DCMAKE_RANLIB=${CMAKE_RANLIB}"
          ${umbra_deps_cmake_args}
          "-DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}/tsan"
          "-DCMAKE_C_FLAGS=-march=${umbra_deps_arch} -fsanitize=thread"
          "-DCMAKE_CXX_FLAGS=-march=${umbra_deps_arch} -stdlib=libc++ -fsanitize=thread"
          "-DCMAKE_SHARED_LINKER_FLAGS=-Wl,--enable-new-dtags -fsanitize=thread"
          -DCMAKE_EXPORT_COMPILE_COMMANDS=OFF
          ${umbra_deps_hwloc_args}
          -C "${CMAKE_CURRENT_LIST_DIR}/../config/oneTBB.cmake"
  COMMAND ${CMAKE_COMMAND} --build "${umbra_tbb_tsan_dir}"
  COMMAND ${CMAKE_COMMAND} -E env ${umbra_tbb_tsan_install_env}
          ${CMAKE_COMMAND} --install "${umbra_tbb_tsan_dir}"
  DEPENDEES install
  USES_TERMINAL TRUE)

# The static clangd index over all dependency sources. It is rebuilt whenever
# the layer is.
#
# A header-only dependency compiles nothing and so writes no compile database.
# The layer writes one for it instead, with an entry per header under the
# directory named by umbra_deps_headers_<name>, each parsed as a C++ header
# with the flags umbra compiles it with.
set(umbra_deps_index "${UMBRA_STAGING_DIR}${CMAKE_INSTALL_PREFIX}/share/clangd/deps.idx")
set(umbra_deps_header_only openqmc)
set(umbra_deps_headers_openqmc include)
set(umbra_deps_databases "")
foreach(dependency IN LISTS umbra_deps)
  if(NOT dependency IN_LIST umbra_deps_header_only)
    ExternalProject_Get_Property(${dependency} BINARY_DIR)
    list(APPEND umbra_deps_databases "${BINARY_DIR}/compile_commands.json")
    continue()
  endif()
  ExternalProject_Get_Property(${dependency} SOURCE_DIR)
  set(include_dir "${SOURCE_DIR}/${umbra_deps_headers_${dependency}}")
  file(GLOB_RECURSE headers LIST_DIRECTORIES false "${include_dir}/*.h")
  list(SORT headers)
  set(entries "")
  foreach(header IN LISTS headers)
    if(NOT entries STREQUAL "")
      string(APPEND entries ",\n")
    endif()
    string(APPEND entries
      "  {\"directory\": \"${include_dir}\", \"file\": \"${header}\", "
      "\"arguments\": [\"${CMAKE_CXX_COMPILER}\", \"-std=c++23\", "
      "\"-march=${umbra_deps_arch}\", \"-stdlib=libc++\", "
      "\"-I${include_dir}\", \"-x\", \"c++-header\", "
      "\"-fsyntax-only\", \"${header}\"]}")
  endforeach()
  set(database "${CMAKE_CURRENT_BINARY_DIR}/clangd-headers/${dependency}/compile_commands.json")
  file(WRITE "${database}" "[\n${entries}\n]\n")
  list(APPEND umbra_deps_databases "${database}")
endforeach()
list(JOIN umbra_deps_databases "|" umbra_deps_databases)
add_custom_target(deps-clangd-index
  COMMAND ${CMAKE_COMMAND}
          "-DINDEXER=${UMBRA_CLANGD_INDEXER}"
          "-DDATABASES=${umbra_deps_databases}"
          "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/clangd-index"
          "-DOUTPUT=${umbra_deps_index}"
          -P "${UMBRA_THIRD_PARTY_DIR}/layers/deps/BuildClangdIndex.cmake"
  COMMENT "Indexing the dependency sources for clangd"
  USES_TERMINAL
  VERBATIM)
add_dependencies(deps-clangd-index ${umbra_deps})

umbra_add_layer(deps DEPENDENCIES ${umbra_deps} deps-clangd-index)
