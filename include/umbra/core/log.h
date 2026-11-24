// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// umbra's diagnostics (docs/adr/0002-cli-diagnostics-on-stderr.md): one line
// per message on stderr, prefixed with its level,
//
//   fatal: cannot open 'scene.usda': No such file or directory
//   info: gpu: OptiX 9.1, device 0: RTX 5090 (sm_120)
//   12:34:56.789012 src/gpu/module.cc:161   trace: gpu: frame 12: 38.2 ms
//
// with the local time and the source line in front of trace lines,
// filtered by level, per component. Results go to stdout and are not
// diagnostics. The UMBRA_LOG_* macros format their message only when its
// level is enabled for its component.

#ifndef UMBRA_CORE_LOG_H_
#define UMBRA_CORE_LOG_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <source_location>
#include <string>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::core {

// The level of a message, from the most to the least severe. A threshold
// enables its level and the more severe ones.
enum class LogLevel : std::uint8_t {
  // The error umbra gives up on, before it exits: always shown.
  kFatal,
  kError,
  kWarning,
  kInfo,
  kDebug,
  kTrace,
};

// The part of umbra, or the library, a message comes from.
enum class LogComponent : std::uint8_t {
  kCore,
  kGpu,
  kUsd,
  kOsl,
  kOiio,
  kOcio,
  kVdb,
  kOsd,
  kEmbree,
};

inline constexpr std::size_t kLogComponentCount = 9;

// The name of a level as it appears in a line and in a filter ("warning").
[[nodiscard]] std::string_view LogLevelName(LogLevel level);

// The name of a component as it appears in a line and in a filter ("gpu").
[[nodiscard]] std::string_view LogComponentName(LogComponent component);

// Whether trace messages exist in this build: only debug builds have them.
[[nodiscard]] bool IsTraceCompiled();

// The threshold of every component, and where its messages go: a
// destination is empty for stderr, a file descriptor from 2 to 9 ("3"), or
// the absolute path of a file the messages are appended to.
class LogFilter {
 public:
  // Every component at `threshold`, to stderr.
  explicit LogFilter(LogLevel threshold = LogLevel::kWarning);

  [[nodiscard]] LogLevel threshold(LogComponent component) const;
  [[nodiscard]] std::string_view destination(LogComponent component) const;

  // Sets the threshold of every component.
  void SetThreshold(LogLevel threshold);
  // Sets the threshold of one component.
  void SetThreshold(LogComponent component, LogLevel threshold);
  // Sets the destination of every component.
  void SetDestination(std::string_view destination);
  // Sets the destination of one component.
  void SetDestination(LogComponent component, std::string_view destination);

 private:
  std::array<LogLevel, kLogComponentCount> thresholds_{};
  std::array<std::string, kLogComponentCount> destinations_{};
};

// Parses a filter in UMBRA_LOG's syntax: comma-separated entries, each a level
// for every component ("debug") or a component and its level ("gpu=trace"),
// optionally followed by a destination ("gpu=trace:/tmp/gpu.log",
// "debug:3"), applied in order onto `base`; empty entries are ignored. Levels
// are error, warning (or warn), info, debug and trace. Fails with
// kInvalidArgument, naming the bad entry.
[[nodiscard]] std::expected<LogFilter, Error> ParseLogFilter(
    std::string_view spec, const LogFilter& base = LogFilter());

// Where a message comes from: a source file, relative to the repository, and
// a line in it.
struct LogOrigin {
  std::string_view file;
  std::uint32_t line = 0;
};

// Formats `message` as diagnostics lines: each line of the message gets the
// "<level>: " prefix, and "<component>: " from info down, and ends with a
// newline. A trace line starts with `time` ("12:34:56.789012") and the
// origin, padded to column 40. A trailing newline of the message adds no
// empty line.
[[nodiscard]] std::string FormatLogLines(LogLevel level, LogComponent component,
                                         std::string_view message,
                                         std::string_view time,
                                         LogOrigin origin);

// Configures the process's diagnostics, opening the files the filter names.
// Call it once, at startup, before other threads log. Fails with
// kUnavailable when a file cannot be opened.
[[nodiscard]] std::expected<void, Error> ConfigureLogging(
    const LogFilter& filter);

// Whether a message of `level` from `component` is shown.
[[nodiscard]] bool IsLogEnabled(LogComponent component, LogLevel level);

// Writes `message` to stderr, when its level is enabled, with one write so
// that lines of concurrent messages do not interleave; a trace line names
// `origin`. For callers that already have the message, such as the
// libraries' log callbacks and the GPU module; code that formats one uses the
// UMBRA_LOG_* macros.
void LogAt(LogLevel level, LogComponent component, std::string_view message,
           LogOrigin origin);

// LogAt, from the caller's line.
inline void Log(
    LogLevel level, LogComponent component, std::string_view message,
    std::source_location location = std::source_location::current()) {
  LogAt(level, component, message,
        {.file = location.file_name(), .line = location.line()});
}

}  // namespace umbra::core

// Logs a std::format message at a level, from a component, given by the
// names of their enumerators: UMBRA_LOG_INFO(kGpu, "{} GPUs", count). The
// arguments are not evaluated when the level is disabled.
// NOLINTBEGIN(cppcoreguidelines-macro-usage): a function would format its
// arguments even when the level is disabled.
#define UMBRA_LOG_AT(level, component, ...)                            \
  (::umbra::core::IsLogEnabled(::umbra::core::LogComponent::component, \
                               ::umbra::core::LogLevel::level)         \
       ? ::umbra::core::Log(::umbra::core::LogLevel::level,            \
                            ::umbra::core::LogComponent::component,    \
                            std::format(__VA_ARGS__))                  \
       : void())

#define UMBRA_LOG_ERROR(component, ...) \
  UMBRA_LOG_AT(kError, component, __VA_ARGS__)
#define UMBRA_LOG_WARNING(component, ...) \
  UMBRA_LOG_AT(kWarning, component, __VA_ARGS__)
#define UMBRA_LOG_INFO(component, ...) \
  UMBRA_LOG_AT(kInfo, component, __VA_ARGS__)
#define UMBRA_LOG_DEBUG(component, ...) \
  UMBRA_LOG_AT(kDebug, component, __VA_ARGS__)

// Trace messages are type-checked in every build but exist only in debug
// builds.
#ifdef NDEBUG
#define UMBRA_LOG_TRACE(component, ...) \
  (false ? UMBRA_LOG_AT(kTrace, component, __VA_ARGS__) : void())
#else
#define UMBRA_LOG_TRACE(component, ...) \
  UMBRA_LOG_AT(kTrace, component, __VA_ARGS__)
#endif
// NOLINTEND(cppcoreguidelines-macro-usage)

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_CORE_LOG_H_
