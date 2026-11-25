# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for OpenSubdiv (cmake -C): osdCPU, with the oneTBB evaluator,
# and osdGPU, with the CUDA evaluator, each as a shared and a static library,
# without examples, tutorials, tests or documentation. The common settings of
# the layer and the CUDA compiler come from layers/deps.cmake.
#
# OpenUSD's pxOsd uses osdCPU. osdGPU refines and evaluates limit surfaces on
# the GPU, into device memory that OptiX builds from: the GPU module links the
# static osdGPU, so that the module holds the process's only CUDA runtime
# (patches/OpenSubdiv links cudart_static). The headers use no exceptions or
# RTTI: errors go to Far::SetErrorCallback.

set(NO_EXAMPLES ON CACHE BOOL "")
set(NO_TUTORIALS ON CACHE BOOL "")
set(NO_REGRESSION ON CACHE BOOL "")
set(NO_TESTS ON CACHE BOOL "")
set(NO_GLTESTS ON CACHE BOOL "")
set(NO_DOC ON CACHE BOOL "")
set(NO_PTEX ON CACHE BOOL "")
set(NO_GLFW ON CACHE BOOL "")
set(NO_GLFW_X11 ON CACHE BOOL "")
set(NO_GLEW ON CACHE BOOL "")

# Compute backends: oneTBB on the CPU, sharing the process's thread pool, and
# CUDA on the GPU. OpenSubdiv drops a backend it cannot find without a word,
# so both are required. No OpenMP (a second thread pool) and none of the
# graphics APIs or their patch shader sources, which serve hardware
# tessellation in a rasterizer.
set(NO_TBB OFF CACHE BOOL "")
set(CMAKE_REQUIRE_FIND_PACKAGE_TBB ON CACHE BOOL "")
set(NO_CUDA OFF CACHE BOOL "")
set(CMAKE_REQUIRE_FIND_PACKAGE_CUDAToolkit ON CACHE BOOL "")
set(NO_OMP ON CACHE BOOL "")
set(NO_OPENCL ON CACHE BOOL "")
set(NO_CLEW ON CACHE BOOL "")
set(NO_OPENGL ON CACHE BOOL "")
set(NO_METAL ON CACHE BOOL "")
set(NO_DX ON CACHE BOOL "")
set(OSD_PATCH_SHADER_SOURCE_GLSL OFF CACHE BOOL "")
set(OSD_PATCH_SHADER_SOURCE_HLSL OFF CACHE BOOL "")
set(OSD_PATCH_SHADER_SOURCE_MSL OFF CACHE BOOL "")

# Exact derivatives of the Gregory patches around extraordinary vertices,
# whose rational blending weights the default leaves out: normals there match
# the surface, on the CPU and the GPU alike.
set(OPENSUBDIV_GREGORY_EVAL_TRUE_DERIVATIVES ON CACHE BOOL "")
