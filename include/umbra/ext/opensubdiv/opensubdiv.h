// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The bridge to OpenSubdiv, which refines umbra's subdivision surfaces: its
// errors and warnings go to umbra's log as the osd component.

#ifndef UMBRA_EXT_OPENSUBDIV_OPENSUBDIV_H_
#define UMBRA_EXT_OPENSUBDIV_OPENSUBDIV_H_

#include <expected>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::opensubdiv {

// Initializes OpenSubdiv for the process: routes its errors and warnings into
// umbra's log and logs its version at info. Call it once at startup, after
// core::ConfigureLogging.
[[nodiscard]] std::expected<void, core::Error> Initialize();

}  // namespace umbra::ext::opensubdiv

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OPENSUBDIV_OPENSUBDIV_H_
