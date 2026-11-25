// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The bridge to OpenVDB, which reads umbra's volumes.

#ifndef UMBRA_EXT_OPENVDB_OPENVDB_H_
#define UMBRA_EXT_OPENVDB_OPENVDB_H_

#include <expected>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::openvdb {

// Initializes OpenVDB for the process: registers its grid types and logs its
// version at info. Call it once at startup, after core::ConfigureLogging.
[[nodiscard]] std::expected<void, core::Error> Initialize();

}  // namespace umbra::ext::openvdb

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OPENVDB_OPENVDB_H_
