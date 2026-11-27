// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The bridge's log: through the function SetLog was handed, if any.

#ifndef UMBRA_EXT_CUDA_LOG_H_
#define UMBRA_EXT_CUDA_LOG_H_

#include "umbra/core/log_hook.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::cuda {

// The bridge's one log hook, which SetLog sets.
[[nodiscard]] core::LogHook& Logging();

}  // namespace umbra::ext::cuda

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_CUDA_LOG_H_
