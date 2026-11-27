// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/optix/errors.h"

#include <optix_host.h>
#include <optix_stubs.h>
#include <optix_types.h>

#include <expected>
#include <format>
#include <string_view>

#include "log.h"
#include "umbra/core/log.h"
#include "umbra/core/log_hook.h"
#include "umbra/core/safety.h"

namespace umbra::ext::optix {

Result<void> Check(OptixResult result, std::string_view what) {
  if (result == OPTIX_SUCCESS) {
    return {};
  }
  return std::unexpected(std::format("OptiX: {}: {} ({})", what,
                                     optixGetErrorString(result),
                                     optixGetErrorName(result)));
}

core::LogHook& Logging() {
  static core::LogHook hook;
  return hook;
}

void SetLog(core::LogFunction UMBRA_NULLABLE log, core::LogLevel threshold) {
  Logging().Set(log, threshold);
}

}  // namespace umbra::ext::optix
