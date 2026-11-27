# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# The GPU's render against the CPU's, through the GPU module of the build
# tree: they need a GPU.

umbra_add_test(umbra_gpu_tests SOURCES module_test.cc DEPENDS gpu device::cpu ext::embree
               LABELS gpu)
target_compile_definitions(umbra_gpu_tests PRIVATE
                           UMBRA_GPU_MODULE_PATH="$<TARGET_FILE:umbra_gpu_backend>")
add_dependencies(umbra_gpu_tests umbra_gpu_backend)
