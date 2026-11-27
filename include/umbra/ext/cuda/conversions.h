// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Conversions between the CUDA runtime's types and those of the driver,
// which OptiX takes.

#ifndef UMBRA_EXT_CUDA_CONVERSIONS_H_
#define UMBRA_EXT_CUDA_CONVERSIONS_H_

#include <cuda.h>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::cuda {

// The pointer the CUDA runtime takes for `address`, on the GPU.
[[nodiscard]] void* PointerOf(CUdeviceptr address);

}  // namespace umbra::ext::cuda

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_CUDA_CONVERSIONS_H_
