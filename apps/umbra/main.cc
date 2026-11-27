// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The umbra executable:
//
//   umbra [--version] [-h | --help] [-q | --quiet] <command> [<args>]
//
// `umbra devices` lists the devices it can render on, with their hardware
// (see device_listing.h); `umbra render [--devices=cpu0,gpu0]
// [--settings=<prim>] [--frames=<start>..<end>] [--step=<step>] <scene>`
// chooses among them (devices.h), shows them on stderr, and renders each
// frame of the scene (frames.h) through Hydra:
// it commits what changed since the last to the devices, which build their
// acceleration structures from it, has them render each render product,
// denoises the color of the products that ask for it through Open Image
// Denoise, on the chosen GPU of the lowest CUDA ordinal or else on the CPU,
// and writes the products as OpenEXR files (product_file.h). `umbra version
// [--build-options]` (or `umbra --version`) shows the version, and
// `umbra help [<command>]` (or -h, before or after a command) the help.
// Without a command, umbra shows its help and fails; -q silences it.
//
// Diagnostics go to stderr (docs/adr/0002-cli-diagnostics-on-stderr.md):
// errors, warnings and info are shown, -q shows errors only, and the
// UMBRA_LOG variable shows debug and trace messages, on stderr or in a file.
// The progress of each phase of a render goes there too, and -q hides it.
// umbra exits with 0 on success, 1 on failure and 2 on a usage error.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "cli/parser.h"
#include "device_listing.h"
#include "devices.h"
#include "frames.h"
#include "product_file.h"
#include "tev.h"
#include "umbra/core/build_info.h"
#include "umbra/core/error.h"
#include "umbra/core/floating_point.h"
#include "umbra/core/log.h"
#include "umbra/core/progress.h"
#include "umbra/core/safety.h"
#include "umbra/core/version.h"
#include "umbra/delegate/session.h"
#include "umbra/ext/embree/embree.h"
#include "umbra/ext/ocio/ocio.h"
#include "umbra/ext/oidn/oidn.h"
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
using ::umbra::core::LogFilter;
using ::umbra::core::LogLevel;

constexpr int kExitSuccess = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

constexpr OptionSpec kHelpOption =
    umbra::cli::FlagOption('h', "help", "show this help");

constexpr std::array<OptionSpec, 3> kGlobalOptions = {
    umbra::cli::FlagOption('\0', "version", "show the version"),
    kHelpOption,
    umbra::cli::FlagOption('q', "quiet", "show errors only"),
};

constexpr CommandSpec kProgram = {
    .name = "umbra",
    .summary = "umbra, a physically based path tracer.",
    .options = kGlobalOptions,
    .arguments = {},
};

constexpr std::array<OptionSpec, 1> kHelpOnly = {kHelpOption};

constexpr std::array<OptionSpec, 6> kRenderOptions = {
    umbra::cli::ValueOption('\0', "devices", "list", {},
                            "the devices to render on (default: all)"),
    umbra::cli::ValueOption('\0', "settings", "prim", {},
                            "the RenderSettings prim to render with "
                            "(default: the stage's)"),
    umbra::cli::ValueOption('\0', "frames", "start..end", {},
                            "the time codes to render, both included, or one "
                            "(default: the stage's start to end)"),
    umbra::cli::ValueOption('\0', "step", "step", {},
                            "the time codes between two frames (default: 1)"),
    umbra::cli::ValueOption('\0', "display", "tev[:host[:port]]", {},
                            "also show the frames in tev as they render "
                            "(default port: 14158)"),
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
        .summary = "Render a scene on the chosen devices",
        .options = kRenderOptions,
        .arguments = "<scene>",
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

// The help of umbra: its usage and commands, then how to choose its
// diagnostics.
std::string ProgramHelp() {
  return umbra::cli::FormatHelp(kProgram, kCommands) +
         "\nUMBRA_LOG shows debug or trace messages, of every component "
         "(debug) or one\n(gpu=trace), on stderr or in a file "
         "(gpu=trace:/tmp/gpu.log). -q shows\nerrors only.\n";
}

// The value of an environment variable, or empty when it is unset.
std::string_view Environment(const char* name) {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read at startup, before threads.
  const char* value = std::getenv(name);
  return value == nullptr ? std::string_view() : std::string_view(value);
}

// The filter of -q and UMBRA_LOG: info and warnings, unless -q hides them,
// and the debug and trace messages UMBRA_LOG enables.
std::expected<LogFilter, Error> LoggingFilter(const ParsedCommand& global) {
  const LogFilter base(global.Count("quiet") > 0 ? LogLevel::kError
                                                 : LogLevel::kInfo);
  return umbra::core::ParseLogFilter(Environment("UMBRA_LOG"), base);
}

// Whether `filter` asks for trace messages from any component.
bool HasTraceThreshold(const LogFilter& filter) {
  for (std::size_t index = 0; index < umbra::core::kLogComponentCount;
       ++index) {
    if (filter.threshold(static_cast<umbra::core::LogComponent>(index)) ==
        LogLevel::kTrace) {
      return true;
    }
  }
  return false;
}

// Configures diagnostics and progress from the global options.
std::expected<void, Error> ConfigureLogging(const ParsedCommand& global) {
  const auto filter = LoggingFilter(global);
  if (!filter.has_value()) {
    return std::unexpected(filter.error());
  }
  if (auto configured = umbra::core::ConfigureLogging(*filter);
      !configured.has_value()) {
    return configured;
  }
  umbra::core::ConfigureProgress(global.Count("quiet") > 0
                                     ? umbra::core::ProgressDisplay::kHidden
                                     : umbra::core::ProgressDisplay::kShown);
  if (HasTraceThreshold(*filter) && !umbra::core::IsTraceCompiled()) {
    UMBRA_LOG_WARNING(kCore,
                      "trace messages exist in debug builds only; this is a "
                      "release build");
  }
  return {};
}

// The commit umbra was built from, abbreviated to 12 hex digits, with
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

// `umbra --version` and `umbra version`: the version, and with
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
      .and_then(umbra::ext::oidn::Initialize)
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

// Reports the failure umbra gives up on, as "fatal:", and returns its
// exit code.
int Failure(const Error& error) {
  umbra::core::Log(LogLevel::kFatal, umbra::core::LogComponent::kCore,
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
                      std::span<const umbra::devices::RenderDevice> chosen) {
  const umbra::core::BuildInfo build = umbra::core::GetBuildInfo();
  std::print(stderr, "umbra version {} ({}, {})\n{}", umbra::core::Version(),
             build.build_type, build.arch,
             umbra::devices::FormatRenderDevices(hardware, chosen));
}

// What `umbra render` renders: its scene, how to load it, its frames, and
// the devices it renders them on and the display it shows them on.
struct RenderRequest {
  std::string_view scene;
  umbra::delegate::LoadOptions options;
  // The frames of --frames, or none for the stage's.
  std::optional<umbra::FrameRange> frames;
  double step = 1.0;
  // The devices of --devices, or empty for all of them.
  std::string_view devices;
  // The tev of --display, if any.
  std::optional<umbra::TevAddress> display;
};

// The scene `render` renders: its one argument.
std::expected<std::string_view, Error> SceneOf(const ParsedCommand& render) {
  const auto scenes = render.arguments();
  if (scenes.size() != 1) {
    return std::unexpected(Error(
        ErrorCode::kInvalidArgument,
        scenes.empty() ? std::string("missing the scene to render; see "
                                     "'umbra render -h'")
                       : std::format("umbra render renders one scene, not {}; "
                                     "see 'umbra render -h'",
                                     scenes.size())));
  }
  return scenes.front();
}

// The tev of `render`'s --display, if it has one.
std::expected<std::optional<umbra::TevAddress>, Error> DisplayOf(
    const ParsedCommand& render) {
  if (render.Count("display") == 0) {
    return std::nullopt;
  }
  return umbra::ParseDisplay(render.Value("display"));
}

// Reads `render`'s --frames and --step into `request`. Fails when either is
// malformed, or they give more frames than one process renders.
std::expected<void, Error> ParseFrameOptions(const ParsedCommand& render,
                                             RenderRequest& request) {
  if (render.Count("frames") > 0) {
    const auto frames = umbra::ParseFrames(render.Value("frames"));
    if (!frames.has_value()) {
      return std::unexpected(frames.error());
    }
    request.frames = *frames;
  }
  if (render.Count("step") > 0) {
    const auto step = umbra::ParseStep(render.Value("step"));
    if (!step.has_value()) {
      return std::unexpected(step.error());
    }
    request.step = *step;
  }
  if (!request.frames.has_value()) {
    return {};
  }
  return umbra::Frames::Of(*request.frames, request.step)
      .transform([](const umbra::Frames&) {});
}

// The scene and options of `umbra render`: exactly one scene.
std::expected<RenderRequest, Error> ParseRender(const ParsedCommand& render) {
  const auto scene = SceneOf(render);
  if (!scene.has_value()) {
    return std::unexpected(scene.error());
  }
  auto display = DisplayOf(render);
  if (!display.has_value()) {
    return std::unexpected(display.error());
  }
  RenderRequest request{
      .scene = *scene,
      .options = {.settings = std::string(render.Value("settings"))},
      .frames = std::nullopt,
      .step = 1.0,
      .devices = render.Value("devices"),
      .display = std::move(*display),
  };
  if (auto parsed = ParseFrameOptions(render, request); !parsed.has_value()) {
    return std::unexpected(parsed.error());
  }
  return request;
}

// Renders `frame` in `session` and writes its products.
std::expected<void, Error> RenderAndWrite(umbra::delegate::Session& session,
                                          std::optional<double> frame) {
  if (auto rendered = session.RenderFrame(frame); !rendered.has_value()) {
    return rendered;
  }
  for (const umbra::delegate::RenderedProduct& product : session.Rendered()) {
    if (auto written = umbra::WriteProduct(product, frame);
        !written.has_value()) {
      return written;
    }
  }
  return {};
}

// Renders each frame `request` renders of its scene on `devices` and writes
// its products.
int RenderFrames(const RenderRequest& request,
                 const umbra::devices::CreatedDevices& devices) {
  std::optional<umbra::TevDisplay> display;
  if (request.display.has_value()) {
    display.emplace(*request.display);
  }
  const auto session = umbra::delegate::OpenSession(
      request.scene, request.options, devices.devices, devices.gpus,
      display.has_value() ? &*display : nullptr);
  if (!session.has_value()) {
    return Failure(session.error());
  }
  const auto frames =
      umbra::FramesOf(request.frames, (*session)->time_codes(), request.step);
  if (!frames.has_value()) {
    return Failure(frames.error());
  }
  for (std::uint64_t index = 0; index < frames->count(); ++index) {
    if (auto rendered = RenderAndWrite(**session, frames->At(index));
        !rendered.has_value()) {
      return Failure(rendered.error());
    }
  }
  return kExitSuccess;
}

// `umbra render`: resolves the devices, shows them, renders each frame of
// the scene on them and writes its products.
int Render(const ParsedCommand& global, const RenderRequest& request) {
  const auto hardware = umbra::devices::ReadHardware();
  if (!hardware.has_value()) {
    return Failure(hardware.error());
  }
  const auto chosen = umbra::devices::ResolveDevices(
      request.devices, umbra::devices::RenderDevices(*hardware));
  if (!chosen.has_value()) {
    return UsageError(chosen.error());
  }
  if (global.Count("quiet") == 0) {
    PrintRenderBlock(*hardware, *chosen);
  }
  const auto created = umbra::devices::CreateDevices(*chosen);
  if (!created.has_value()) {
    return Failure(created.error());
  }
  return RenderFrames(request, *created);
}

// `umbra help [<command>]`: the help of umbra, or of a command.
int Help(const ParsedCommand& help) {
  if (help.arguments().empty()) {
    std::print("{}", ProgramHelp());
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

// Runs a command that works on the hardware, once its arguments are checked
// and the libraries are initialized.
int RunOnHardware(const ParsedCommand& global, const ParsedCommand& command) {
  std::optional<RenderRequest> request;
  if (command.name() == "render") {
    auto parsed = ParseRender(command);
    if (!parsed.has_value()) {
      return UsageError(parsed.error());
    }
    request = std::move(*parsed);
  }
  if (auto initialized = InitializeLibraries(); !initialized.has_value()) {
    return Failure(initialized.error());
  }
  if (request.has_value()) {
    return Render(global, *request);
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
    // A bad UMBRA_LOG or flag is a usage error; a log file that cannot be
    // opened is a failure.
    return configured.error().code() == ErrorCode::kInvalidArgument
               ? UsageError(configured.error())
               : Failure(configured.error());
  }
  if (global.Count("help") > 0) {
    std::print("{}", ProgramHelp());
    return kExitSuccess;
  }
  if (global.Count("version") > 0) {
    PrintVersion(global);
    return kExitSuccess;
  }
  if (!command_line->command.has_value()) {
    // The help, and a failure, since there was nothing to do.
    if (global.Count("quiet") == 0) {
      std::print("{}", ProgramHelp());
    }
    return kExitFailure;
  }
  return Run(global, *command_line->command);
}
