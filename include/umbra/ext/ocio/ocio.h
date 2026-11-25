// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The bridge to OpenColorIO, umbra's color management: its diagnostics go to
// umbra's log as the ocio component, and the process's current config is the
// one $OCIO names or, without it, OpenColorIO's built-in default config.

#ifndef UMBRA_EXT_OCIO_OCIO_H_
#define UMBRA_EXT_OCIO_OCIO_H_

#include <expected>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::ocio {

// Initializes OpenColorIO for the process: routes its messages into umbra's
// log at the ocio component's threshold, loads and validates the config, and
// logs the versions at info. Call it once at startup, after
// core::ConfigureLogging. Fails with kUnavailable when the config cannot be
// loaded or is invalid.
[[nodiscard]] std::expected<void, core::Error> Initialize();

}  // namespace umbra::ext::ocio

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OCIO_OCIO_H_
