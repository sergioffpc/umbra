# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for Open Image Denoise (cmake -C): the shared core library and
# its two devices, which the core loads as modules at run time: the CPU device,
# whose kernels ISPC compiles and which runs on the layer's oneTBB, and the
# CUDA device, whose convolutions are CUTLASS's. Only the RT filter's weights,
# for path-traced images, without the RTLightmap filter, the apps, SYCL or
# HIP. The common settings of the layer come from layers/deps.cmake.
#
# The weights come from the nested weights submodule, in Git LFS, and
# CUTLASS from the nested external/cutlass submodule (see
# forge/checkout_submodules.py). ISPC comes from docker/toolchain.Dockerfile.

set(OIDN_STATIC_LIB OFF CACHE BOOL "")
set(OIDN_APPS OFF CACHE BOOL "")
set(OIDN_FILTER_RT ON CACHE BOOL "")
set(OIDN_FILTER_RTLIGHTMAP OFF CACHE BOOL "")
set(OIDN_INSTALL_DEPENDENCIES OFF CACHE BOOL "")

set(OIDN_DEVICE_CPU ON CACHE BOOL "")
set(OIDN_DEVICE_SYCL OFF CACHE BOOL "")
set(OIDN_DEVICE_HIP OFF CACHE BOOL "")

# The CUDA device calls the driver API only, through the driver's libcuda.so.1
# from the host: it links no CUDA runtime, so the process keeps the one the
# GPU module links statically. patches/oidn builds it for sm_120 and hands
# its own build the layer's compilers and flags.
set(OIDN_DEVICE_CUDA ON CACHE BOOL "")
set(OIDN_DEVICE_CUDA_API Driver CACHE STRING "")
