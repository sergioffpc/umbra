// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "support.h"

#include <atomic>
#include <string_view>

#include "umbra/core/log.h"
#include "umbra/device/gpu/module_abi.h"

namespace umbra::device::gpu::backend {

LogState& Logging() {
  static LogState state;
  return state;
}

void LogFromBridge(core::LogLevel level, std::string_view message,
                   core::LogOrigin origin) {
  const ModuleLogFunction log = Logging().log.load(std::memory_order_relaxed);
  if (log == nullptr ||
      level > Logging().threshold.load(std::memory_order_relaxed)) {
    return;
  }
  log(level, message, origin);
}

}  // namespace umbra::device::gpu::backend
