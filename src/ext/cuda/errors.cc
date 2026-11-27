// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/cuda/errors.h"

#include <cuda_runtime_api.h>
#include <driver_types.h>

#include <expected>
#include <format>
#include <string_view>

#include "log.h"
#include "umbra/core/log.h"
#include "umbra/core/log_hook.h"
#include "umbra/core/safety.h"

namespace umbra::ext::cuda {

Result<void> Check(cudaError_t error, std::string_view what) {
  if (error == cudaSuccess) {
    return {};
  }
  return std::unexpected(std::format("CUDA: {}: {} ({})", what,
                                     cudaGetErrorString(error),
                                     cudaGetErrorName(error)));
}

core::LogHook& Logging() {
  static core::LogHook hook;
  return hook;
}

void SetLog(core::LogFunction UMBRA_NULLABLE log, core::LogLevel threshold) {
  Logging().Set(log, threshold);
}

}  // namespace umbra::ext::cuda
