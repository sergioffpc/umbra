// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/openvdb/openvdb.h"

#include <openvdb/openvdb.h>
#include <openvdb/version.h>

#include <exception>
#include <expected>
#include <format>

#include "umbra/core/error.h"
#include "umbra/core/log.h"

namespace umbra::ext::openvdb {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

std::expected<void, Error> Initialize() {
  try {
    ::openvdb::initialize();
  } catch (const std::exception& error) {
    return std::unexpected(
        Error(ErrorCode::kUnavailable,
              std::format("OpenVDB: cannot initialize: {}", error.what())));
  }
  UMBRA_LOG_INFO(kVdb, "OpenVDB {}",
                 ::openvdb::OPENVDB_VERSION_NAME::getLibraryVersionString());
  return {};
}

}  // namespace umbra::ext::openvdb
