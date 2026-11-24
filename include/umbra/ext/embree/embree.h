// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The bridge to Embree, umbra's CPU ray tracer. The process has one Embree
// device, which runs on the oneTBB pool (Embree's tasking system is TBB) and
// reports its errors to umbra's log as the embree component.

#ifndef UMBRA_EXT_EMBREE_EMBREE_H_
#define UMBRA_EXT_EMBREE_EMBREE_H_

#include <expected>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::embree {

// Initializes Embree for the process: creates its device, routes the device's
// errors into umbra's log, and logs the version at info. Call it once at
// startup, after core::ConfigureLogging. Fails with kUnavailable when the
// device cannot be created (an unsupported CPU, say).
[[nodiscard]] std::expected<void, core::Error> Initialize();

}  // namespace umbra::ext::embree

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_EMBREE_EMBREE_H_
