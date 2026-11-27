// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/cuda/errors.h"

#include <cuda_runtime_api.h>
#include <driver_types.h>

#include <atomic>
#include <expected>
#include <format>
#include <source_location>
#include <string_view>

#include "log.h"
#include "umbra/core/log.h"
#include "umbra/core/safety.h"

namespace umbra::ext::cuda {
namespace {

// The function SetLog was handed.
std::atomic<LogFunction>& Hook() {
  static std::atomic<LogFunction> hook{nullptr};
  return hook;
}

}  // namespace

Result<void> Check(cudaError_t error, std::string_view what) {
  if (error == cudaSuccess) {
    return {};
  }
  return std::unexpected(std::format("CUDA: {}: {} ({})", what,
                                     cudaGetErrorString(error),
                                     cudaGetErrorName(error)));
}

void SetLog(LogFunction UMBRA_NULLABLE log) {
  Hook().store(log, std::memory_order_relaxed);
}

void Log(core::LogLevel level, std::string_view message,
         std::source_location location) {
  if (const LogFunction log = Hook().load(std::memory_order_relaxed);
      log != nullptr) {
    log(level, message,
        {.file = location.file_name(), .line = location.line()});
  }
}

}  // namespace umbra::ext::cuda
