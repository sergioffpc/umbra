# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# umbra goes to bin/ of the build tree, so that it finds the GPU module in
# lib/ as it does when installed.

umbra_add_executable(umbra SOURCES main.cc DEPENDS core gpu)
target_compile_definitions(umbra PRIVATE
                           UMBRA_HOMEPAGE_URL="${PROJECT_HOMEPAGE_URL}")
set_target_properties(umbra PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
if(UMBRA_GPU_ENABLED)
  add_dependencies(umbra umbra_gpu_backend)
endif()
