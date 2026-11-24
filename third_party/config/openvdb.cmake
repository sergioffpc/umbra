# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for OpenVDB (cmake -C): the shared core library, which reads
# and writes .vdb files and edits their grids, and NanoVDB, the header-only
# read-only grids umbra traces volumes with on the GPU. Without the tools,
# AX, the bindings, the plugins, the tests or the docs. The common settings
# of the layer come from layers/deps.cmake.
#
# OpenVDB no longer needs Boost, Blosc or log4cplus: it embeds its subset of
# Blosc and LZ4. It finds oneTBB and Imath in the layer, zlib and zstd in
# Ubuntu (see third_party/versions.md).

set(OPENVDB_BUILD_CORE ON CACHE BOOL "")
set(OPENVDB_CORE_SHARED ON CACHE BOOL "")
set(OPENVDB_CORE_STATIC OFF CACHE BOOL "")
set(OPENVDB_BUILD_BINARIES OFF CACHE BOOL "")
set(OPENVDB_BUILD_AX OFF CACHE BOOL "")
set(OPENVDB_BUILD_PYTHON_MODULE OFF CACHE BOOL "")
set(OPENVDB_BUILD_UNITTESTS OFF CACHE BOOL "")
set(OPENVDB_BUILD_DOCS OFF CACHE BOOL "")
set(OPENVDB_BUILD_HOUDINI_PLUGIN OFF CACHE BOOL "")
set(OPENVDB_BUILD_MAYA_PLUGIN OFF CACHE BOOL "")
set(OPENVDB_ENABLE_UNINSTALL OFF CACHE BOOL "")
set(USE_CCACHE OFF CACHE BOOL "")
set(USE_PKGCONFIG OFF CACHE BOOL "")
set(CONCURRENT_MALLOC None CACHE STRING "")
# OpenVDB deprecates C++17, its default. Its own RPATH logic adds absolute
# paths; off, the layer's $ORIGIN applies.
set(CMAKE_CXX_STANDARD 20 CACHE STRING "")
set(OPENVDB_ENABLE_RPATH OFF CACHE BOOL "")

# half is Imath's, the type OpenEXR and the rest of the layer use. OpenEXR
# itself is only for OpenVDB's tools.
set(USE_IMATH_HALF ON CACHE BOOL "")
set(USE_EXR OFF CACHE BOOL "")
set(USE_PNG OFF CACHE BOOL "")
set(USE_ZLIB ON CACHE BOOL "")
set(USE_ZSTD ON CACHE BOOL "")

# SIMD: the point rasterizers, the PCA of points and the finite-difference
# stencils use explicit AVX2 vectors through the vendored VectorClass (VCL),
# at x86-64-v3, the layer's ISA (8 = AVX2). version.h records both, so umbra
# compiles the same vectors; it installs VCL's headers under openvdb/ext/vcl.
set(USE_VCL ON CACHE BOOL "")
set(OPENVDB_X86_INSTRSET 8 CACHE STRING "")

# NanoVDB: grids converted from OpenVDB's on the CPU, in parallel on oneTBB,
# and its CUDA headers (device grids, GPU builders). Its CUDA path finds
# CCCL in the CUDA toolkit and links the static CUDA runtime (patches/).
set(OPENVDB_BUILD_NANOVDB ON CACHE BOOL "")
set(USE_NANOVDB ON CACHE BOOL "")
set(NANOVDB_BUILD_TOOLS OFF CACHE BOOL "")
set(NANOVDB_BUILD_UNITTESTS OFF CACHE BOOL "")
set(NANOVDB_BUILD_EXAMPLES OFF CACHE BOOL "")
set(NANOVDB_BUILD_PYTHON_MODULE OFF CACHE BOOL "")
set(NANOVDB_USE_OPENVDB ON CACHE BOOL "")
set(NANOVDB_USE_CUDA ON CACHE BOOL "")
set(NANOVDB_USE_TBB ON CACHE BOOL "")
set(NANOVDB_USE_ZLIB ON CACHE BOOL "")
set(NANOVDB_USE_BLOSC OFF CACHE BOOL "")
set(NANOVDB_USE_INTRINSICS ON CACHE BOOL "")
set(NANOVDB_USE_MAGICAVOXEL OFF CACHE BOOL "")
