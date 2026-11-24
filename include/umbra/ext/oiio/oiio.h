// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The bridge to OpenImageIO, which reads and writes umbra's images: it runs
// on the oneTBB pool (docs/adr/0001-onetbb-thread-pool.md), with OpenEXR's own
// thread pool off.

#ifndef UMBRA_EXT_OIIO_OIIO_H_
#define UMBRA_EXT_OIIO_OIIO_H_

#include <expected>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::oiio {

// Initializes OpenImageIO for the process: parallel loops on oneTBB, no
// OpenEXR thread pool, and its version logged at info. Call it once at
// startup, after core::ConfigureLogging. Fails with kUnavailable when
// OpenImageIO rejects a setting.
[[nodiscard]] std::expected<void, core::Error> Initialize();

}  // namespace umbra::ext::oiio

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OIIO_OIIO_H_
