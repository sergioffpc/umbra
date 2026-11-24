// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The umbra executable. Its commands and options follow git's:
//
//   umbra [--version] [-h | --help] [-q | --quiet] [-v | --verbose]
//         [--color=<when>] <command> [<args>]
//
// `umbra devices` lists the devices it can render on, with their hardware
// (see devices.h); `umbra render [--devices=cpu0,gpu0]` chooses among them
// and shows them on stderr, and renders nothing yet. `umbra version
// [--build-options]` (or `umbra --version`) and `umbra help [<command>]`
// (or -h, before or after a command) answer as git's do. Without a command,
// umbra shows its help and fails, as git does; -q silences it.
//
// Diagnostics go to stderr (docs/adr/0002-cli-diagnostics-on-stderr.md):
// -q, -v, -vv and -vvv and the UMBRA_LOG variable set their level, and
// --color whether they are colored. umbra exits with 0 on success, 1 on
// failure and 2 on a usage error.

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cli/parser.h"
#include "devices.h"
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
#include "umbra/ext/osl/osl.h"
#include "umbra/ext/usd/usd.h"

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

constexpr OptionSpec kHelpOption =
    umbra::cli::FlagOption('h', "help", "show this help");

constexpr std::array<OptionSpec, 5> kGlobalOptions = {
    umbra::cli::FlagOption('\0', "version", "show the version"),
    kHelpOption,
    umbra::cli::FlagOption('q', "quiet", "show errors only"),
    umbra::cli::FlagOption('v', "verbose",
                           "show more: -v info, -vv debug, -vvv trace"),
    umbra::cli::ValueOption('\0', "color", "when", kColorChoices,
                            "color diagnostics: auto, always or never"),
};

constexpr CommandSpec kProgram = {
    .name = "umbra",
    .summary = "umbra, a physically based path tracer.",
    .options = kGlobalOptions,
    .arguments = {},
};

constexpr std::array<OptionSpec, 1> kHelpOnly = {kHelpOption};

constexpr std::array<OptionSpec, 2> kRenderOptions = {
    umbra::cli::ValueOption('\0', "devices", "list", {},
                            "the devices to render on (default: all)"),
    kHelpOption,
};

constexpr std::array<OptionSpec, 2> kVersionOptions = {
    umbra::cli::FlagOption('\0', "build-options", "also show build options"),
    kHelpOption,
};

constexpr std::array<CommandSpec, 4> kCommands = {
    CommandSpec{
        .name = "devices",
        .summary = "List the devices umbra can render on",
        .options = kHelpOnly,
        .arguments = {},
    },
    CommandSpec{
        .name = "render",
        .summary = "Render on the chosen devices",
        .options = kRenderOptions,
        .arguments = {},
    },
    CommandSpec{
        .name = "version",
        .summary = "Display version information",
        .options = kVersionOptions,
        .arguments = {},
    },
    CommandSpec{
        .name = "help",
        .summary = "Display help information about umbra",
        .options = kHelpOnly,
        .arguments = "[<command>]",
    },
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

// The commit umbra was built from, short as git abbreviates it, with
// "-dirty" kept; empty when unknown.
std::string ShortCommit(std::string_view commit) {
  constexpr std::size_t kShortLength = 12;
  constexpr std::string_view kDirty = "-dirty";
  if (commit == "unknown" || commit.empty()) {
    return {};
  }
  const std::string_view suffix =
      commit.ends_with(kDirty) ? kDirty : std::string_view();
  return std::string(commit.substr(0, kShortLength)) + std::string(suffix);
}

// `umbra --version` and `umbra version`, as git's: the version, and with
// --build-options how umbra was built.
void PrintVersion(const ParsedCommand& version) {
  std::println("umbra version {}", umbra::core::Version());
  if (version.Count("build-options") == 0) {
    return;
  }
  const umbra::core::BuildInfo build = umbra::core::GetBuildInfo();
  std::println("cpu: {}", build.arch);
  const std::string commit = ShortCommit(build.commit);
  if (commit.empty()) {
    std::println("no commit associated with this build");
  } else {
    std::println("built from commit: {}", commit);
  }
  std::println("build type: {}", build.build_type);
  std::println("compiler: {}", build.compiler);
}

// Initializes the libraries umbra uses, in order, once diagnostics are
// configured, so that their messages reach umbra's log
// (docs/adr/0002-cli-diagnostics-on-stderr.md).
std::expected<void, Error> InitializeLibraries() {
  return umbra::ext::ocio::Initialize()
      .and_then(umbra::ext::oiio::Initialize)
      .and_then(umbra::ext::openvdb::Initialize)
      .and_then(umbra::ext::opensubdiv::Initialize)
      .and_then(umbra::ext::embree::Initialize)
      .and_then(umbra::ext::usd::Initialize)
      .and_then(umbra::ext::osl::Initialize);
}

// Reports a usage error and returns its exit code.
int UsageError(const Error& error) {
  umbra::core::Log(LogLevel::kError, umbra::core::LogComponent::kCore,
                   error.message());
  return kExitUsage;
}

// Reports a failure and returns its exit code.
int Failure(const Error& error) {
  umbra::core::Log(LogLevel::kError, umbra::core::LogComponent::kCore,
                   error.message());
  return kExitFailure;
}

// `umbra devices`: the devices and their hardware, on stdout.
int ListDevices() {
  const auto hardware = umbra::devices::ReadHardware();
  if (!hardware.has_value()) {
    return Failure(hardware.error());
  }
  std::print("{}", umbra::devices::FormatHardware(*hardware));
  return kExitSuccess;
}

// The block a render starts with, on stderr: umbra's version and build, and
// the devices it renders on. -q hides it.
void PrintRenderBlock(const umbra::devices::Hardware& hardware,
                      std::span<const umbra::devices::Device> chosen) {
  const umbra::core::BuildInfo build = umbra::core::GetBuildInfo();
  std::print(stderr, "umbra version {} ({}, {})\n{}", umbra::core::Version(),
             build.build_type, build.arch,
             umbra::devices::FormatRenderDevices(hardware, chosen));
}

// `umbra render`: resolves the devices and shows them. It renders nothing
// yet.
int Render(const ParsedCommand& global, const ParsedCommand& render) {
  const auto hardware = umbra::devices::ReadHardware();
  if (!hardware.has_value()) {
    return Failure(hardware.error());
  }
  const std::vector<umbra::devices::Device> devices =
      umbra::devices::RenderDevices(*hardware);
  const auto chosen =
      umbra::devices::ResolveDevices(render.Value("devices"), devices);
  if (!chosen.has_value()) {
    return UsageError(chosen.error());
  }
  if (global.Count("quiet") == 0) {
    PrintRenderBlock(*hardware, *chosen);
  }
  return kExitSuccess;
}

// `umbra help [<command>]`: the help of umbra, or of a command.
int Help(const ParsedCommand& help) {
  if (help.arguments().empty()) {
    std::print("{}", umbra::cli::FormatHelp(kProgram, kCommands));
    return kExitSuccess;
  }
  const std::string_view name = help.arguments().front();
  const auto* const command =
      std::ranges::find(kCommands, name, &CommandSpec::name);
  if (command == kCommands.end()) {
    return UsageError(umbra::cli::NotACommand(kProgram, name));
  }
  std::print("{}", umbra::cli::FormatCommandHelp(kProgram, *command));
  return kExitSuccess;
}

// Runs a command that works on the hardware, once the libraries are
// initialized.
int RunOnHardware(const ParsedCommand& global, const ParsedCommand& command) {
  if (auto initialized = InitializeLibraries(); !initialized.has_value()) {
    return Failure(initialized.error());
  }
  if (command.name() == "render") {
    return Render(global, command);
  }
  return ListDevices();
}

// Runs the command, or shows its help when -h is given.
int Run(const ParsedCommand& global, const ParsedCommand& command) {
  if (command.Count("help") > 0) {
    const auto* const spec =
        std::ranges::find(kCommands, command.name(), &CommandSpec::name);
    std::print("{}", umbra::cli::FormatCommandHelp(kProgram, *spec));
    return kExitSuccess;
  }
  if (command.name() == "help") {
    return Help(command);
  }
  if (command.name() == "version") {
    PrintVersion(command);
    return kExitSuccess;
  }
  return RunOnHardware(global, command);
}

}  // namespace

int main(int argc, char** argv) {
  umbra::core::ConfigureThreadFloatingPoint();

  // SAFETY: the C runtime passes argc arguments in argv.
  UMBRA_UNSAFE_BUFFER_BEGIN
  const std::span<const char* const> args(argv, static_cast<std::size_t>(argc));
  UMBRA_UNSAFE_BUFFER_END
  const auto command_line =
      umbra::cli::Parse(kProgram, kCommands, args.subspan(1));
  if (!command_line.has_value()) {
    return UsageError(command_line.error());
  }
  const ParsedCommand& global = command_line->global;
  if (auto configured = ConfigureLogging(global); !configured.has_value()) {
    return UsageError(configured.error());
  }
  if (global.Count("help") > 0) {
    std::print("{}", umbra::cli::FormatHelp(kProgram, kCommands));
    return kExitSuccess;
  }
  if (global.Count("version") > 0) {
    PrintVersion(global);
    return kExitSuccess;
  }
  if (!command_line->command.has_value()) {
    // As git does: the help, and a failure, since there was nothing to do.
    if (global.Count("quiet") == 0) {
      std::print("{}", umbra::cli::FormatHelp(kProgram, kCommands));
    }
    return kExitFailure;
  }
  return Run(global, *command_line->command);
}
