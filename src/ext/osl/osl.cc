// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/osl/osl.h"

#include <OSL/oslversion.h>
// The definition of ErrorHandler, which LogErrorHandler derives from; the
// name reaches OIIO:: through nsversions.h.
#include <OpenImageIO/errorhandler.h>  // IWYU pragma: keep
#include <OpenImageIO/nsversions.h>

#include <expected>
#include <string>

#include "umbra/core/error.h"
#include "umbra/core/log.h"

namespace umbra::ext::osl {
namespace {

using ::umbra::core::LogComponent;
using ::umbra::core::LogLevel;

// umbra's level for an OpenImageIO error code, which OSL's shading systems
// report with.
LogLevel LevelOf(int code) {
  switch (code) {
    case OIIO::ErrorHandler::EH_ERROR:
    case OIIO::ErrorHandler::EH_SEVERE:
      return LogLevel::kError;
    case OIIO::ErrorHandler::EH_WARNING:
      return LogLevel::kWarning;
    case OIIO::ErrorHandler::EH_DEBUG:
      return LogLevel::kDebug;
    default:
      return LogLevel::kInfo;
  }
}

// The error handler of the process's shading systems: every message goes to
// umbra's log, which filters it by level.
class LogErrorHandler final : public OIIO::ErrorHandler {
 public:
  void operator()(int code, const std::string& message) override {
    core::Log(LevelOf(code), LogComponent::kOsl, message);
  }
};

}  // namespace

std::expected<void, core::Error> Initialize() {
  // Created once, for the shading systems to come; it outlives them all.
  static LogErrorHandler handler;
  handler.verbosity(core::IsLogEnabled(LogComponent::kOsl, LogLevel::kDebug)
                        ? OIIO::ErrorHandler::VERBOSE
                        : OIIO::ErrorHandler::NORMAL);
  UMBRA_LOG_INFO(kOsl, "OSL {}", OSL_LIBRARY_VERSION_STRING);
  return {};
}

}  // namespace umbra::ext::osl
