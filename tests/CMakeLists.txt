# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# GoogleTest tests, one directory per module, mirroring src/.

add_subdirectory(core)
add_subdirectory(delegate)
add_subdirectory(device)
if(UMBRA_GPU_ENABLED)
  add_subdirectory(gpu)
endif()
