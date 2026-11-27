// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The bridge to Open Image Denoise, which the render delegate denoises
// through: its devices, buffers and filters are owned through `owners.h`,
// and its errors are returned, never printed, since OIDN's own output is
// off.

#ifndef UMBRA_EXT_OIDN_OIDN_H_
#define UMBRA_EXT_OIDN_OIDN_H_

#include <expected>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::oidn {

// Initializes Open Image Denoise for the process: logs its version at debug.
// Call it once at startup, after core::ConfigureLogging.
[[nodiscard]] std::expected<void, core::Error> Initialize();

}  // namespace umbra::ext::oidn

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OIDN_OIDN_H_
