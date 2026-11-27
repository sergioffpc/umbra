// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/cuda/conversions.h"

#include <cuda.h>

#include <bit>

namespace umbra::ext::cuda {

void* PointerOf(CUdeviceptr address) { return std::bit_cast<void*>(address); }

}  // namespace umbra::ext::cuda
