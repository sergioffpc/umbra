// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/core/log.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "umbra/core/error.h"

namespace umbra::core {
namespace {

constexpr std::array<std::string_view, kLogComponentCount> kComponentNames = {
    "core", "gpu", "usd", "osl", "oiio", "ocio", "vdb", "osd", "embree",
};

constexpr std::array<LogLevel, 5> kLevels = {
    LogLevel::kError, LogLevel::kWarning, LogLevel::kInfo,
    LogLevel::kDebug, LogLevel::kTrace,
};

constexpr std::size_t Index(LogComponent component) {
  return static_cast<std::size_t>(component);
}

// The thresholds of the process's diagnostics. Each is read on every log call
// and written only by ConfigureLogging, so relaxed atomics suffice.
struct LoggingState {
  std::array<std::atomic<LogLevel>, kLogComponentCount> thresholds{};

  LoggingState() {
    for (auto& threshold : thresholds) {
      threshold.store(LogLevel::kWarning, std::memory_order_relaxed);
    }
  }
};

LoggingState& State() {
  static LoggingState state;
  return state;
}

std::optional<LogLevel> ParseLevel(std::string_view name) {
  if (name == "warn") {
    return LogLevel::kWarning;
  }
  const auto* const found = std::ranges::find(kLevels, name, LogLevelName);
  if (found == kLevels.end()) {
    return std::nullopt;
  }
  return *found;
}

std::optional<LogComponent> ParseComponent(std::string_view name) {
  const auto* const found = std::ranges::find(kComponentNames, name);
  if (found == kComponentNames.end()) {
    return std::nullopt;
  }
  return static_cast<LogComponent>(
      std::ranges::distance(kComponentNames.begin(), found));
}

Error BadEntry(std::string_view entry, std::string_view reason) {
  return {ErrorCode::kInvalidArgument,
          std::format("invalid log filter entry '{}': {}", entry, reason)};
}

// Applies one entry of a filter ("debug" or "gpu=trace") onto `filter`.
std::expected<void, Error> ApplyEntry(std::string_view entry,
                                      LogFilter& filter) {
  if (entry.empty()) {
    return {};
  }
  const std::size_t equals = entry.find('=');
  const std::string_view level_name =
      equals == std::string_view::npos ? entry : entry.substr(equals + 1);
  const std::optional<LogLevel> level = ParseLevel(level_name);
  if (!level.has_value()) {
    return std::unexpected(
        BadEntry(entry, "levels are error, warning, info, debug and trace"));
  }
  if (equals == std::string_view::npos) {
    filter.SetThreshold(*level);
    return {};
  }
  const std::optional<LogComponent> component =
      ParseComponent(entry.substr(0, equals));
  if (!component.has_value()) {
    return std::unexpected(
        BadEntry(entry,
                 "components are core, gpu, usd, osl, oiio, ocio, vdb, osd and "
                 "embree"));
  }
  filter.SetThreshold(*component, *level);
  return {};
}

// The prefix of every line of a message: "error: ", "info: gpu: ".
std::string LinePrefix(LogLevel level, LogComponent component) {
  if (level >= LogLevel::kInfo) {
    return std::format("{}: {}: ", LogLevelName(level),
                       LogComponentName(component));
  }
  return std::format("{}: ", LogLevelName(level));
}

// Writes all of `text` to stderr. A diagnostic that cannot be written is
// dropped: there is nowhere left to report it.
void WriteToStderr(std::string_view text) {
  while (!text.empty()) {
    const auto written = write(STDERR_FILENO, text.data(), text.size());
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return;
    }
    text.remove_prefix(static_cast<std::size_t>(written));
  }
}

}  // namespace

std::string_view LogLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kFatal:
      return "fatal";
    case LogLevel::kError:
      return "error";
    case LogLevel::kWarning:
      return "warning";
    case LogLevel::kInfo:
      return "info";
    case LogLevel::kDebug:
      return "debug";
    case LogLevel::kTrace:
      return "trace";
  }
  return "unknown";
}

std::string_view LogComponentName(LogComponent component) {
  return kComponentNames.at(Index(component));
}

bool IsTraceCompiled() {
#ifdef NDEBUG
  return false;
#else
  return true;
#endif
}

LogFilter::LogFilter(LogLevel threshold) { thresholds_.fill(threshold); }

LogLevel LogFilter::threshold(LogComponent component) const {
  return thresholds_.at(Index(component));
}

void LogFilter::SetThreshold(LogLevel threshold) {
  thresholds_.fill(threshold);
}

void LogFilter::SetThreshold(LogComponent component, LogLevel threshold) {
  thresholds_.at(Index(component)) = threshold;
}

std::expected<LogFilter, Error> ParseLogFilter(std::string_view spec,
                                               const LogFilter& base) {
  LogFilter filter = base;
  while (!spec.empty()) {
    const std::size_t comma = spec.find(',');
    const std::string_view entry = spec.substr(0, comma);
    if (auto applied = ApplyEntry(entry, filter); !applied.has_value()) {
      return std::unexpected(std::move(applied.error()));
    }
    spec = comma == std::string_view::npos ? std::string_view()
                                           : spec.substr(comma + 1);
  }
  return filter;
}

std::string FormatLogLines(LogLevel level, LogComponent component,
                           std::string_view message) {
  const std::string prefix = LinePrefix(level, component);
  if (message.ends_with('\n')) {
    message.remove_suffix(1);
  }
  std::string lines;
  while (true) {
    const std::size_t newline = message.find('\n');
    std::format_to(std::back_inserter(lines), "{}{}\n", prefix,
                   message.substr(0, newline));
    if (newline == std::string_view::npos) {
      return lines;
    }
    message.remove_prefix(newline + 1);
  }
}

void ConfigureLogging(const LogFilter& filter) {
  LoggingState& state = State();
  for (std::size_t index = 0; index < kLogComponentCount; ++index) {
    state.thresholds.at(index).store(
        filter.threshold(static_cast<LogComponent>(index)),
        std::memory_order_relaxed);
  }
}

bool IsLogEnabled(LogComponent component, LogLevel level) {
  return level <= State()
                      .thresholds.at(Index(component))
                      .load(std::memory_order_relaxed);
}

void Log(LogLevel level, LogComponent component, std::string_view message) {
  if (!IsLogEnabled(component, level)) {
    return;
  }
  WriteToStderr(FormatLogLines(level, component, message));
}

}  // namespace umbra::core
