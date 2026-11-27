// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The driver's OptiX runtime, which the bridge loads.

#ifndef UMBRA_EXT_OPTIX_OPTIX_H_
#define UMBRA_EXT_OPTIX_OPTIX_H_

#include "umbra/core/safety.h"
#include "umbra/ext/optix/errors.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::optix {

// Loads the driver's OptiX runtime, once per process: every later call
// returns what the first did. Fails, saying why, when the driver has no
// OptiX runtime or one too old for the headers umbra is built with.
[[nodiscard]] Result<void> Initialize();

}  // namespace umbra::ext::optix

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OPTIX_OPTIX_H_
