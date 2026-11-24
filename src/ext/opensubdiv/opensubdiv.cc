// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/opensubdiv/opensubdiv.h"

#include <opensubdiv/far/error.h>
#include <opensubdiv/version.h>

#include <expected>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/log.h"

namespace umbra::ext::opensubdiv {
namespace {

namespace far = ::OpenSubdiv::Far;

using ::umbra::core::LogComponent;
using ::umbra::core::LogLevel;

// OpenSubdiv's error callback. Every kind of error it reports is an error to
// umbra; a fatal one ends the process inside OpenSubdiv after this returns.
void LogError(far::ErrorType /*type*/, const char* message) {
  core::Log(LogLevel::kError, LogComponent::kOsd, std::string_view(message));
}

void LogWarning(const char* message) {
  core::Log(LogLevel::kWarning, LogComponent::kOsd, std::string_view(message));
}

}  // namespace

std::expected<void, core::Error> Initialize() {
  far::SetErrorCallback(LogError);
  far::SetWarningCallback(LogWarning);
  UMBRA_LOG_INFO(kOsd, "OpenSubdiv {}.{}.{}", OPENSUBDIV_VERSION_MAJOR,
                 OPENSUBDIV_VERSION_MINOR, OPENSUBDIV_VERSION_PATCH);
  return {};
}

}  // namespace umbra::ext::opensubdiv
