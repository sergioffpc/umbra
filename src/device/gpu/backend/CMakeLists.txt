# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# The GPU module, libumbra-gpu.so, which umbra loads at run time (see
# include/umbra/device/gpu/module.h). It calls the CUDA runtime and OptiX
# through their bridges, umbra::ext::cuda and umbra::ext::optix, which it
# links: the CUDA runtime is linked statically, so it needs no CUDA shared
# library, and optixInit loads the driver's OptiX runtime. It goes to lib/ of
# the build tree, next to bin/umbra.

find_package(OpenQMC CONFIG REQUIRED)

umbra_add_loadable_module(umbra_device_gpu_backend
  SOURCES module.cc accel.cc device.cc held.cc instances.cc light.cc
          pipeline.cc prototype.cc render.cc support.cc
          # The camera model, which the host makes for each render product.
          "${PROJECT_SOURCE_DIR}/src/device/camera_rays.cc"
          "${PROJECT_SOURCE_DIR}/src/device/image.cc"
  HEADERS core device
  LINK umbra::ext::cuda umbra::ext::optix OpenQMC::OpenQMC)
set_target_properties(umbra_device_gpu_backend PROPERTIES
  OUTPUT_NAME umbra-gpu
  LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

# The placeholder integrator's programs, which run the single-source code:
# std::array's members are constexpr host functions, which device code calls
# under --expt-relaxed-constexpr.
umbra_add_optix_programs(umbra_device_gpu_backend SOURCES placeholder.cu
                         FUNCTION PlaceholderPrograms
                         NAMESPACE umbra::device::gpu::backend)
target_link_libraries(umbra_device_gpu_backend_PlaceholderPrograms_ptx PRIVATE
                      OpenQMC::OpenQMC)
target_compile_options(umbra_device_gpu_backend_PlaceholderPrograms_ptx PRIVATE
                       --expt-relaxed-constexpr -Werror=all-warnings)
