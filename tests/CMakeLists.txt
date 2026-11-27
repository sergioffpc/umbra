# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# GoogleTest tests, one directory per module, mirroring src/.

add_subdirectory(core)
add_subdirectory(device)
add_subdirectory(ext/usd)
if(UMBRA_GPU_ENABLED)
  add_subdirectory(gpu)
endif()
