# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# The CUDA bridge, linked only into the GPU module (src/device/gpu/backend):
# it links the CUDA runtime statically, so the module needs no CUDA shared
# library: cudart loads the driver, libcuda.so.1, from the host when it is
# first called.

umbra_add_ext_module(
  cuda
  LOADABLE
  SOURCES
  conversions.cc
  errors.cc
  owners.cc
  runtime.cc
  HEADERS
  core
  LINK
  CUDA::cudart_static)
