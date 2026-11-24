// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/ocio/ocio.h"

#include <OpenColorIO/OpenColorABI.h>
#include <OpenColorIO/OpenColorIO.h>
#include <OpenColorIO/OpenColorTypes.h>

#include <array>
#include <exception>
#include <expected>
#include <format>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/log.h"

namespace umbra::ext::ocio {
namespace {

namespace colorio = OCIO_NAMESPACE;

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;
using ::umbra::core::LogComponent;
using ::umbra::core::LogLevel;

// The built-in config umbra uses when $OCIO is not set.
constexpr const char* kDefaultConfig = "ocio://default";

// A prefix OpenColorIO puts on its messages, and its level.
struct Prefix {
  std::string_view text;
  LogLevel level;
};

constexpr auto kPrefixes = std::to_array<Prefix>({
    {.text = "[OpenColorIO Error]: ", .level = LogLevel::kError},
    {.text = "[OpenColorIO Warning]: ", .level = LogLevel::kWarning},
    {.text = "[OpenColorIO Info]: ", .level = LogLevel::kInfo},
    {.text = "[OpenColorIO Debug]: ", .level = LogLevel::kDebug},
});

// OpenColorIO's logging function: one line per call, with its level in a
// prefix, which umbra's own line replaces.
void LogFromOcio(const char* message) {
  std::string_view text(message);
  LogLevel level = LogLevel::kInfo;
  for (const Prefix& prefix : kPrefixes) {
    if (text.starts_with(prefix.text)) {
      text.remove_prefix(prefix.text.size());
      level = prefix.level;
      break;
    }
  }
  core::Log(level, LogComponent::kOcio, text);
}

// OpenColorIO's level for the ocio component's threshold. OpenColorIO has
// no error level of its own: errors come with warnings.
colorio::LoggingLevel OcioLoggingLevel() {
  if (core::IsLogEnabled(LogComponent::kOcio, LogLevel::kDebug)) {
    return colorio::LOGGING_LEVEL_DEBUG;
  }
  if (core::IsLogEnabled(LogComponent::kOcio, LogLevel::kInfo)) {
    return colorio::LOGGING_LEVEL_INFO;
  }
  return colorio::LOGGING_LEVEL_WARNING;
}

// The config $OCIO names, or the built-in default one.
colorio::ConstConfigRcPtr LoadConfig() {
  if (colorio::IsEnvVariablePresent("OCIO") &&
      !std::string_view(colorio::GetEnvVariable("OCIO")).empty()) {
    return colorio::Config::CreateFromEnv();
  }
  return colorio::Config::CreateFromBuiltinConfig(kDefaultConfig);
}

}  // namespace

std::expected<void, Error> Initialize() {
  colorio::SetLoggingFunction(LogFromOcio);
  colorio::SetLoggingLevel(OcioLoggingLevel());
  try {
    const colorio::ConstConfigRcPtr config = LoadConfig();
    config->validate();
    colorio::SetCurrentConfig(config);
    UMBRA_LOG_INFO(kOcio, "OpenColorIO {}, config '{}'", colorio::GetVersion(),
                   config->getName());
  } catch (const std::exception& error) {
    return std::unexpected(Error(
        ErrorCode::kUnavailable,
        std::format("OpenColorIO: cannot load the config: {}", error.what())));
  }
  return {};
}

}  // namespace umbra::ext::ocio
