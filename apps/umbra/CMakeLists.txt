# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# umbra goes to bin/ of the build tree, so that it finds the GPU module in
# lib/ as it does when installed.

umbra_add_executable(umbra
                     SOURCES main.cc channels.cc cli/parser.cc cpu_check.cc
                             device_listing.cc devices.cc frames.cc
                             product_file.cc tev.cc
                     DEPENDS core delegate device device::cpu device::gpu ext::ocio
                             ext::oidn ext::oiio ext::openvdb ext::opensubdiv
                             ext::embree ext::usd ext::osl)
# The CPU check runs before anything else, on any x86-64 CPU (cpu_check.cc):
# the last -march wins over the one of every build.
set_source_files_properties(cpu_check.cc PROPERTIES
                            COMPILE_OPTIONS -march=x86-64
                            COMPILE_DEFINITIONS "UMBRA_ARCH=\"${UMBRA_ARCH}\"")
target_include_directories(umbra PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(umbra PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
if(UMBRA_GPU_ENABLED)
  add_dependencies(umbra umbra_device_gpu_backend)
endif()
