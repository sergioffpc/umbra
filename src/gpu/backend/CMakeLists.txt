# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# The GPU module, libumbra-gpu.so, which umbra loads at run time (see
# include/umbra/gpu/module.h). It links the CUDA runtime statically, so it
# needs no CUDA shared library: cudart loads the driver, libcuda.so.1, from
# the host when the module first calls it, and optixInit loads the driver's
# OptiX runtime. It goes to lib/ of the build tree, next to bin/umbra.

umbra_add_loadable_module(umbra_gpu_backend
  SOURCES module.cc
  HEADERS core gpu
  LINK CUDA::cudart_static umbra::optix)
set_target_properties(umbra_gpu_backend PROPERTIES
  OUTPUT_NAME umbra-gpu
  LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
