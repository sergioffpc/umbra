# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# The loader of the GPU module, linked into umbra, and (with UMBRA_GPU) the
# GPU module itself, libumbra-gpu.so, in backend/.

umbra_add_module(gpu SOURCES module.cc DEPENDS core device LINK ${CMAKE_DL_LIBS})

if(UMBRA_GPU_ENABLED)
  add_subdirectory(backend)
endif()
