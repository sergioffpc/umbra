// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The bridge to OSL, which runs umbra's shaders: the shading systems it will
// create report to umbra's log as the osl component.

#ifndef UMBRA_EXT_OSL_OSL_H_
#define UMBRA_EXT_OSL_OSL_H_

#include <expected>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::osl {

// Initializes OSL for the process: creates the error handler that routes the
// messages of its shading systems into umbra's log, and logs its version at
// info. Call it once at startup, after core::ConfigureLogging.
[[nodiscard]] std::expected<void, core::Error> Initialize();

// The widest batch OSL can shade on this CPU: 16 (AVX-512) or 8 (AVX2) lanes,
// with fused multiply-add, or 0 when it can only shade one point at a time.
// Measured once, by configuring a shading system, and remembered.
[[nodiscard]] int BatchWidth();

}  // namespace umbra::ext::osl

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OSL_OSL_H_
