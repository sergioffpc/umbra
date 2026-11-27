// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The bridge's log: through the function SetLog was handed, if any.

#ifndef UMBRA_EXT_CUDA_LOG_H_
#define UMBRA_EXT_CUDA_LOG_H_

#include <source_location>
#include <string_view>

#include "umbra/core/log.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::cuda {

// Logs `message`, from the source line `location` names.
void Log(core::LogLevel level, std::string_view message,
         std::source_location location = std::source_location::current());

}  // namespace umbra::ext::cuda

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_CUDA_LOG_H_
