# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# The OptiX bridge, linked only into the GPU module (src/device/gpu/backend):
# optixInit loads the driver's OptiX runtime, through which every host call
# goes. It sees the CUDA headers for the driver's types that OptiX takes.

umbra_add_ext_module(
  optix
  LOADABLE
  SOURCES
  errors.cc
  host.cc
  optix.cc
  owners.cc
  HEADERS
  core
  LINK
  CUDA::toolkit
  umbra::optix)
