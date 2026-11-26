// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/core/log.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "stderr_output.h"
#include "umbra/core/error.h"

namespace umbra::core {
namespace {

constexpr std::array<std::string_view, kLogComponentCount> kComponentNames = {
    "core", "gpu", "usd", "osl",    "oiio",
    "ocio", "vdb", "osd", "embree", "device",
};

// The levels UMBRA_LOG enables; the others are always shown.
constexpr std::array<LogLevel, 2> kLevels = {
    LogLevel::kDebug,
    LogLevel::kTrace,
};

// The column the message of a trace line starts at, after its time and
// origin.
constexpr std::size_t kTraceMessageColumn = 40;

constexpr std::size_t Index(LogComponent component) {
  return static_cast<std::size_t>(component);
}

// The thresholds and outputs of the process's diagnostics. Each is read on
// every log call and written only by ConfigureLogging, so relaxed atomics
// suffice.
struct LoggingState {
  std::array<std::atomic<LogLevel>, kLogComponentCount> thresholds{};
  // The file descriptor of each component's messages.
  std::array<std::atomic<int>, kLogComponentCount> outputs{};

  LoggingState() {
    for (auto& threshold : thresholds) {
      threshold.store(LogLevel::kInfo, std::memory_order_relaxed);
    }
    for (auto& output : outputs) {
      output.store(STDERR_FILENO, std::memory_order_relaxed);
    }
  }
};

LoggingState& State() {
  static LoggingState state;
  return state;
}

std::optional<LogLevel> ParseLevel(std::string_view name) {
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

// Whether `destination` is one UMBRA_LOG accepts: a file descriptor from 2
// to 9, or an absolute path.
bool IsDestination(std::string_view destination) {
  return (destination.size() == 1 && destination.front() >= '2' &&
          destination.front() <= '9') ||
         destination.starts_with('/');
}

// One entry of a filter, split: "gpu=trace:/tmp/x" is "gpu", "trace" and
// "/tmp/x".
struct Entry {
  std::optional<std::string_view> component;
  std::string_view level;
  std::optional<std::string_view> destination;
};

Entry SplitEntry(std::string_view entry) {
  Entry split;
  if (const std::size_t equals = entry.find('=');
      equals != std::string_view::npos) {
    split.component = entry.substr(0, equals);
    entry.remove_prefix(equals + 1);
  }
  const std::size_t colon = entry.find(':');
  split.level = entry.substr(0, colon);
  if (colon != std::string_view::npos) {
    split.destination = entry.substr(colon + 1);
  }
  return split;
}

// Applies a parsed entry's level and destination onto `filter`, for
// `component` or, without one, for every component.
void Apply(const Entry& entry, LogLevel level,
           std::optional<LogComponent> component, LogFilter& filter) {
  if (component.has_value()) {
    filter.SetThreshold(*component, level);
  } else {
    filter.SetThreshold(level);
  }
  if (!entry.destination.has_value()) {
    return;
  }
  if (component.has_value()) {
    filter.SetDestination(*component, *entry.destination);
  } else {
    filter.SetDestination(*entry.destination);
  }
}

// Applies one entry of a filter ("debug", "gpu=trace" or "gpu=trace:3") onto
// `filter`.
std::expected<void, Error> ApplyEntry(std::string_view entry,
                                      LogFilter& filter) {
  if (entry.empty()) {
    return {};
  }
  const Entry split = SplitEntry(entry);
  const std::optional<LogLevel> level = ParseLevel(split.level);
  if (!level.has_value()) {
    return std::unexpected(BadEntry(entry, "levels are debug and trace"));
  }
  if (split.destination.has_value() && !IsDestination(*split.destination)) {
    return std::unexpected(
        BadEntry(entry,
                 "destinations are a file descriptor from 2 to 9 or an "
                 "absolute path"));
  }
  const std::optional<LogComponent> component =
      split.component.and_then(ParseComponent);
  if (split.component.has_value() && !component.has_value()) {
    return std::unexpected(
        BadEntry(entry,
                 "components are core, gpu, usd, osl, oiio, ocio, vdb, osd, "
                 "embree and device"));
  }
  Apply(split, *level, component, filter);
  return {};
}

// The file descriptor of a destination: stderr when it is empty, the
// descriptor it names, or a file it opens for appending.
std::expected<int, Error> OpenDestination(std::string_view destination) {
  if (destination.empty()) {
    return STDERR_FILENO;
  }
  if (!destination.starts_with('/')) {
    return destination.front() - '0';
  }
  const std::string path(destination);
  constexpr int kMode = 0644;
  constexpr int kFlags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open is variadic.
  const int file = open(path.c_str(), kFlags, kMode);
  if (file < 0) {
    return std::unexpected(
        Error(ErrorCode::kUnavailable,
              std::format("cannot open the log file '{}': {}", path,
                          std::generic_category().message(errno))));
  }
  return file;
}

// The prefix of every line of a message: none for info, which is for the
// user; "error: " for fatal errors, errors and warnings; "debug: gpu: "; and
// for a trace line "12:34:56.789012 src/gpu/module.cc:161   trace: gpu: ".
std::string LinePrefix(LogLevel level, LogComponent component,
                       std::string_view time, LogOrigin origin) {
  if (level == LogLevel::kInfo) {
    return {};
  }
  if (level < LogLevel::kInfo) {
    return std::format("{}: ", LogLevelName(level));
  }
  std::string prefix;
  if (level == LogLevel::kTrace) {
    prefix = std::format("{} {}:{} ", time, origin.file, origin.line);
    prefix.resize(std::max(prefix.size(), kTraceMessageColumn), ' ');
  }
  std::format_to(std::back_inserter(prefix), "{}: {}: ", LogLevelName(level),
                 LogComponentName(component));
  return prefix;
}

// The local time of day, to the microsecond: "12:34:56.789012".
std::string TimeOfDay() {
  std::timespec now{};
  if (std::timespec_get(&now, TIME_UTC) == 0) {
    return "--:--:--.------";
  }
  std::tm local{};
  localtime_r(&now.tv_sec, &local);
  constexpr std::int64_t kNanosecondsPerMicrosecond = 1000;
  return std::format("{:02}:{:02}:{:02}.{:06}", local.tm_hour, local.tm_min,
                     local.tm_sec,
                     std::int64_t{now.tv_nsec} / kNanosecondsPerMicrosecond);
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

std::string_view LogFilter::destination(LogComponent component) const {
  return destinations_.at(Index(component));
}

void LogFilter::SetDestination(std::string_view destination) {
  destinations_.fill(std::string(destination));
}

void LogFilter::SetDestination(LogComponent component,
                               std::string_view destination) {
  destinations_.at(Index(component)) = destination;
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
                           std::string_view message, std::string_view time,
                           LogOrigin origin) {
  const std::string prefix = LinePrefix(level, component, time, origin);
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

std::expected<void, Error> ConfigureLogging(const LogFilter& filter) {
  LoggingState& state = State();
  // Each file is opened once, however many components write to it.
  std::map<std::string, int, std::less<>> opened;
  for (std::size_t index = 0; index < kLogComponentCount; ++index) {
    const auto component = static_cast<LogComponent>(index);
    const std::string_view destination = filter.destination(component);
    auto output =
        opened.contains(destination)
            ? std::expected<int, Error>(opened.find(destination)->second)
            : OpenDestination(destination);
    if (!output.has_value()) {
      return std::unexpected(std::move(output.error()));
    }
    opened.emplace(destination, *output);
    state.thresholds.at(index).store(filter.threshold(component),
                                     std::memory_order_relaxed);
    state.outputs.at(index).store(*output, std::memory_order_relaxed);
  }
  return {};
}

bool IsLogEnabled(LogComponent component, LogLevel level) {
  return level <= State()
                      .thresholds.at(Index(component))
                      .load(std::memory_order_relaxed);
}

void LogAt(LogLevel level, LogComponent component, std::string_view message,
           LogOrigin origin) {
  if (!IsLogEnabled(component, level)) {
    return;
  }
  const std::string time =
      level == LogLevel::kTrace ? TimeOfDay() : std::string();
  // Only debug and trace lines go where UMBRA_LOG sends them.
  const int output =
      level >= LogLevel::kDebug
          ? State().outputs.at(Index(component)).load(std::memory_order_relaxed)
          : STDERR_FILENO;
  const std::string lines =
      FormatLogLines(level, component, message, time, origin);
  // On stderr, the lines go above the progress line, if one is shown.
  if (output == STDERR_FILENO) {
    internal::WriteAboveProgressLine(lines);
  } else {
    internal::WriteAll(output, lines);
  }
}

}  // namespace umbra::core
