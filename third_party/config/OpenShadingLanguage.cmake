# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for OSL (cmake -C): liboslcomp, liboslexec, liboslquery and
# liboslnoise, shared, with oslc and oslinfo and the standard shaders
# (stdosl.h and the .oso of its library), without the test renderers, the
# OpenImageIO plugin, osltoy, the bindings or the docs. The common settings
# of the layer come from layers/deps.cmake.
#
# OSL JIT-compiles shaders with the toolchain image's LLVM 23 (libLLVM.so,
# libclang-cpp.so, X86 and NVPTX targets). It finds Imath, OpenImageIO,
# pugixml and robin-map in the layer; flex and bison build its compiler.

set(OSL_BUILD_TESTS OFF CACHE BOOL "")
set(OSL_BUILD_PLUGINS OFF CACHE BOOL "")
set(OSL_BUILD_SHADERS ON CACHE BOOL "")
set(INSTALL_DOCS OFF CACHE BOOL "")
set(USE_QT OFF CACHE BOOL "")
set(USE_PYTHON OFF CACHE BOOL "")
set(USE_PARTIO OFF CACHE BOOL "")
set(USE_CCACHE OFF CACHE BOOL "")
set(STOP_ON_WARNING OFF CACHE BOOL "")
set(BUILD_MISSING_DEPS OFF CACHE BOOL "")
set(BUILD_MISSING_ROBINMAP OFF CACHE BOOL "")

# GPU: shaders compiled by LLVM's NVPTX backend to PTX, linked into OptiX
# programs, with the CUDA runtime static. Requires the embedded LLVM bitcode
# of the shadeops and fast math, both defaults.
set(OSL_USE_OPTIX ON CACHE BOOL "")
set(CUDA_PREFER_STATIC_LIBS ON CACHE BOOL "")
set(USE_LLVM_BITCODE ON CACHE BOOL "")
set(USE_FAST_MATH ON CACHE BOOL "")
# The PTX of OSL's shadeops targets the layer's GPU, Blackwell (sm_120).
set(CUDA_TARGET_ARCH sm_120 CACHE STRING "")

# CPU: the layer's x86-64-v3, and batched shading alongside the
# one-point-at-a-time path: 8 points at a time in AVX2 registers, and 16 in
# AVX-512 ones on the CPUs that have it. Each width is its own library
# (lib_b8_AVX2_oslexec, lib_b16_AVX512_oslexec), which liboslexec loads only
# when the CPU supports it and the renderer allows FMA in the JIT
# (llvm_jit_fma).
set(USE_SIMD "avx2,f16c" CACHE STRING "")
set(USE_BATCHED "b8_AVX2,b16_AVX512" CACHE STRING "")
# OIIO's fmath functions in their branch-free forms, which vectorize in the
# batched paths, at some cost to the one-point path and to bit-exactness
# with it. On x86-64-v3 the batched AVX2 path is always available.
set(OIIO_FMATH_SIMD_FRIENDLY ON CACHE BOOL "")
