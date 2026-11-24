// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// umbra's diagnostics (docs/adr/0002-cli-diagnostics-on-stderr.md): one line
// per message on stderr, as a command-line tool reports,
//
//   umbra: error: cannot open 'scene.usda': No such file or directory
//   umbra: info: [gpu] OptiX 9.1, device 0: RTX 5090 (sm_120)
//
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
#include <string>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::core {

// The level of a message, from the most to the least severe. A threshold
// enables its level and the more severe ones.
enum class LogLevel : std::uint8_t {
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

// The threshold of every component.
class LogFilter {
 public:
  // Every component at `threshold`.
  explicit LogFilter(LogLevel threshold = LogLevel::kWarning);

  [[nodiscard]] LogLevel threshold(LogComponent component) const;

  // Sets the threshold of every component.
  void SetThreshold(LogLevel threshold);
  // Sets the threshold of one component.
  void SetThreshold(LogComponent component, LogLevel threshold);

 private:
  std::array<LogLevel, kLogComponentCount> thresholds_{};
};

// Parses a filter in UMBRA_LOG's syntax: comma-separated entries, each a level
// for every component ("debug") or a component and its level ("gpu=trace"),
// applied in order onto `base`; empty entries are ignored. Levels are error,
// warning (or warn), info, debug and trace. Fails with kInvalidArgument,
// naming the bad entry.
[[nodiscard]] std::expected<LogFilter, Error> ParseLogFilter(
    std::string_view spec, const LogFilter& base = LogFilter());

// Whether lines color their level word.
enum class LogColor : std::uint8_t {
  kOff,
  kOn,
};

// Formats `message` as diagnostics lines: each line of the message gets the
// "umbra: <level>: " prefix, and "[component] " from info down, and ends with
// a newline. A trailing newline of the message adds no empty line.
[[nodiscard]] std::string FormatLogLines(LogLevel level, LogComponent component,
                                         std::string_view message,
                                         LogColor color);

// Configures the process's diagnostics: the filter, and whether lines are
// colored. Call it once, at startup, before other threads log.
void ConfigureLogging(const LogFilter& filter, LogColor color);

// Whether a message of `level` from `component` is shown.
[[nodiscard]] bool IsLogEnabled(LogComponent component, LogLevel level);

// Writes `message` to stderr, when its level is enabled, with one write so
// that lines of concurrent messages do not interleave. For callers that
// already have the message, such as the libraries' log callbacks; code that
// formats one uses the UMBRA_LOG_* macros.
void Log(LogLevel level, LogComponent component, std::string_view message);

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
