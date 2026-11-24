// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The umbra executable. It prints its version block, in the style of LLVM's
// tools, and the GPU executors it can run (see include/umbra/gpu/module.h):
//
//   umbra version 0.1.0 (https://github.com/sergioffpc/umbra <commit>)
//   Build: Release, x86-64-v3, clang 23.1.6
//   License: Apache-2.0 — provided "AS IS", WITHOUT WARRANTIES OR ...
//   GPU executors: 1
//
// or, without a usable GPU, "GPU executors: none (<reason>)".
//
// Diagnostics go to stderr (docs/adr/0002-cli-diagnostics-on-stderr.md):
// -q, -v, -vv and -vvv and the UMBRA_LOG variable set their level, and
// --color whether they are colored. umbra exits with 0 on success, 1 on
// failure and 2 on a usage error.

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>

#include "cli/parser.h"
#include "umbra/core/build_info.h"
#include "umbra/core/error.h"
#include "umbra/core/floating_point.h"
#include "umbra/core/log.h"
#include "umbra/core/safety.h"
#include "umbra/core/version.h"
#include "umbra/ext/embree/embree.h"
#include "umbra/ext/ocio/ocio.h"
#include "umbra/ext/oiio/oiio.h"
#include "umbra/ext/opensubdiv/opensubdiv.h"
#include "umbra/ext/openvdb/openvdb.h"
#include "umbra/gpu/module.h"

namespace {

using ::umbra::cli::CommandSpec;
using ::umbra::cli::OptionSpec;
using ::umbra::cli::ParsedCommand;
using ::umbra::core::Error;
using ::umbra::core::ErrorCode;
using ::umbra::core::LogColor;
using ::umbra::core::LogFilter;
using ::umbra::core::LogLevel;

constexpr int kExitSuccess = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

constexpr std::array<std::string_view, 3> kColorChoices = {
    "auto",
    "always",
    "never",
};

constexpr std::array<OptionSpec, 5> kGlobalOptions = {
    umbra::cli::FlagOption('q', "quiet", "show errors only"),
    umbra::cli::FlagOption('v', "verbose",
                           "show more: -v info, -vv debug, -vvv trace"),
    umbra::cli::ValueOption('\0', "color", "when", kColorChoices,
                            "color diagnostics: auto, always or never"),
    umbra::cli::FlagOption('h', "help", "show this help"),
    umbra::cli::FlagOption('\0', "version",
                           "show the version and the GPU executors"),
};

constexpr CommandSpec kProgram = {
    .name = "umbra",
    .summary = "umbra, a physically based path tracer.",
    .options = kGlobalOptions,
    .arguments = {},
};

// The value of an environment variable, or empty when it is unset.
std::string_view Environment(const char* name) {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read at startup, before threads.
  const char* value = std::getenv(name);
  return value == nullptr ? std::string_view() : std::string_view(value);
}

// The entries of a UMBRA_LOG spec that name a component ("gpu=trace"), which
// the -q and -v flags do not override.
std::string ComponentEntries(std::string_view spec) {
  std::string entries;
  while (!spec.empty()) {
    const std::string_view entry = spec.substr(0, spec.find(','));
    spec.remove_prefix(std::min(spec.size(), entry.size() + 1));
    if (entry.contains('=')) {
      entries += entries.empty() ? "" : ",";
      entries += entry;
    }
  }
  return entries;
}

// The global threshold the flags ask for, if they ask for one.
std::expected<std::optional<LogLevel>, Error> FlagThreshold(
    const ParsedCommand& global) {
  const int quiet = global.Count("quiet");
  const int verbose = global.Count("verbose");
  if (quiet > 0 && verbose > 0) {
    return std::unexpected(Error(ErrorCode::kInvalidArgument,
                                 "-q and -v cannot be used together"));
  }
  if (quiet > 0) {
    return LogLevel::kError;
  }
  if (verbose == 0) {
    return std::nullopt;
  }
  if (verbose == 1) {
    return LogLevel::kInfo;
  }
  return verbose == 2 ? LogLevel::kDebug : LogLevel::kTrace;
}

// The filter of the flags and UMBRA_LOG, the flags winning over its global
// part.
std::expected<LogFilter, Error> LoggingFilter(const ParsedCommand& global) {
  return FlagThreshold(global).and_then([](std::optional<LogLevel> threshold) {
    const std::string_view spec = Environment("UMBRA_LOG");
    if (!threshold.has_value()) {
      return umbra::core::ParseLogFilter(spec);
    }
    return umbra::core::ParseLogFilter(ComponentEntries(spec),
                                       LogFilter(*threshold));
  });
}

// Whether diagnostics are colored: --color, else when stderr is a terminal
// and NO_COLOR is unset or empty.
LogColor LoggingColor(const ParsedCommand& global) {
  const std::string_view when = global.Value("color");
  if (when == "always") {
    return LogColor::kOn;
  }
  if (when == "never" || isatty(STDERR_FILENO) == 0 ||
      !Environment("NO_COLOR").empty()) {
    return LogColor::kOff;
  }
  return LogColor::kOn;
}

// Configures diagnostics from the global options.
std::expected<void, Error> ConfigureLogging(const ParsedCommand& global) {
  const auto filter = LoggingFilter(global);
  if (!filter.has_value()) {
    return std::unexpected(filter.error());
  }
  umbra::core::ConfigureLogging(*filter, LoggingColor(global));
  if (global.Count("verbose") > 2 && !umbra::core::IsTraceCompiled()) {
    UMBRA_LOG_WARNING(kCore,
                      "trace messages exist in debug builds only; this is a "
                      "release build");
  }
  return {};
}

void PrintVersion() {
  const umbra::core::BuildInfo build = umbra::core::GetBuildInfo();
  // UMBRA_HOMEPAGE_URL is the homepage of the CMake project, defined by
  // apps/umbra/CMakeLists.txt.
  std::println("umbra version {} ({} {})", umbra::core::Version(),
               UMBRA_HOMEPAGE_URL, build.commit);
  std::println("Build: {}, {}, {}", build.build_type, build.arch,
               build.compiler);
  std::println(
      "License: Apache-2.0 — provided \"AS IS\", WITHOUT WARRANTIES OR "
      "CONDITIONS OF ANY KIND.");

  const auto gpus = umbra::gpu::DefaultModulePath().and_then(
      [](const auto& path) { return umbra::gpu::CountGpuExecutors(path); });
  if (gpus.has_value() && *gpus > 0) {
    std::println("GPU executors: {}", *gpus);
  } else if (gpus.has_value()) {
    std::println("GPU executors: none (no usable GPU)");
  } else {
    std::println("GPU executors: none ({})", gpus.error().message());
  }
}

// Initializes the libraries umbra uses, in order, once diagnostics are
// configured, so that their messages reach umbra's log
// (docs/adr/0002-cli-diagnostics-on-stderr.md).
std::expected<void, Error> InitializeLibraries() {
  return umbra::ext::ocio::Initialize()
      .and_then(umbra::ext::oiio::Initialize)
      .and_then(umbra::ext::openvdb::Initialize)
      .and_then(umbra::ext::opensubdiv::Initialize)
      .and_then(umbra::ext::embree::Initialize);
}

// Reports a usage error and returns its exit code.
int UsageError(const Error& error) {
  umbra::core::Log(LogLevel::kError, umbra::core::LogComponent::kCore,
                   error.message());
  return kExitUsage;
}

}  // namespace

int main(int argc, char** argv) {
  umbra::core::ConfigureThreadFloatingPoint();

  // SAFETY: the C runtime passes argc arguments in argv.
  UMBRA_UNSAFE_BUFFER_BEGIN
  const std::span<const char* const> args(argv, static_cast<std::size_t>(argc));
  UMBRA_UNSAFE_BUFFER_END
  const auto command_line = umbra::cli::Parse(kProgram, {}, args.subspan(1));
  if (!command_line.has_value()) {
    return UsageError(command_line.error());
  }
  const ParsedCommand& global = command_line->global;
  if (auto configured = ConfigureLogging(global); !configured.has_value()) {
    return UsageError(configured.error());
  }
  if (global.Count("help") > 0) {
    std::print("{}", umbra::cli::FormatHelp(kProgram, {}));
    return kExitSuccess;
  }
  if (auto initialized = InitializeLibraries(); !initialized.has_value()) {
    umbra::core::Log(LogLevel::kError, umbra::core::LogComponent::kCore,
                     initialized.error().message());
    return kExitFailure;
  }
  PrintVersion();
  return kExitSuccess;
}
