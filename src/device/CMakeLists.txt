# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

find_package(OpenQMC CONFIG REQUIRED)

umbra_add_module(
  device
  SOURCES
  camera_rays.cc
  device.cc
  diagnostic_device.cc
  film.cc
  image.cc
  DEPENDS
  core
  LINK
  OpenQMC::OpenQMC)

# The single-source code, compiled by nvcc too, though nothing runs it yet:
# a kernel that calls it, built and never linked. std::array's members are
# constexpr host functions, which device code calls under
# --expt-relaxed-constexpr.
if(UMBRA_GPU_ENABLED)
  add_library(umbra_device_kernels OBJECT single_source_check.cu)
  target_include_directories(umbra_device_kernels PRIVATE
                             "${PROJECT_SOURCE_DIR}/include")
  target_link_libraries(umbra_device_kernels PRIVATE OpenQMC::OpenQMC)
  target_compile_options(umbra_device_kernels PRIVATE
                         --expt-relaxed-constexpr -Werror=all-warnings)
endif()
