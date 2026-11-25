// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/usd/usd.h"

#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/callContext.h>
#include <pxr/base/tf/debug.h>
#include <pxr/base/tf/diagnosticLite.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/error.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/base/tf/status.h>
#include <pxr/base/tf/warning.h>
#include <pxr/base/work/threadLimits.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/usd/common.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <exception>
#include <expected>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/core/progress.h"
#include "umbra/scene/scene.h"

namespace umbra::ext::usd {
namespace {

namespace pxr = PXR_NS;

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;
using ::umbra::core::LogComponent;
using ::umbra::core::LogLevel;

// OpenUSD's diagnostics, routed into umbra's log. Tf prints nothing of its
// own once a delegate is registered, except for fatal errors, which it still
// reports and then aborts on. It may call the delegate from any thread.
class LogDelegate final : public pxr::TfDiagnosticMgr::Delegate {
 public:
  void IssueError(const pxr::TfError& error) override {
    if (error.GetQuiet()) {
      return;
    }
    if (error.GetDiagnosticCode() == pxr::TF_DIAGNOSTIC_CODING_ERROR_TYPE) {
      core::Log(
          LogLevel::kError, LogComponent::kUsd,
          std::format("coding error in {}: {}",
                      error.GetContext().GetFunction(), error.GetCommentary()));
      return;
    }
    core::Log(LogLevel::kError, LogComponent::kUsd, error.GetCommentary());
  }

  void IssueFatalError(const pxr::TfCallContext& context,
                       const std::string& message) override {
    core::Log(LogLevel::kFatal, LogComponent::kUsd,
              std::format("in {}: {}", context.GetFunction(), message));
  }

  void IssueStatus(const pxr::TfStatus& status) override {
    if (!status.GetQuiet()) {
      core::Log(LogLevel::kDebug, LogComponent::kUsd, status.GetCommentary());
    }
  }

  void IssueWarning(const pxr::TfWarning& warning) override {
    if (!warning.GetQuiet()) {
      core::Log(LogLevel::kWarning, LogComponent::kUsd,
                warning.GetCommentary());
    }
  }
};

// Registers the process's delegate, once. It is never removed: Tf may report
// until the process exits.
void RouteDiagnostics() {
  static LogDelegate delegate;
  static const bool kRegistered = [] {
    pxr::TfDiagnosticMgr::GetInstance().AddDelegate(&delegate);
    return true;
  }();
  static_cast<void>(kRegistered);
}

// Loads the plugin registry and checks that the file formats are among the
// plugins: without them no stage opens.
std::expected<void, Error> LoadPlugins() {
  const pxr::PlugRegistry& registry = pxr::PlugRegistry::GetInstance();
  if (!pxr::SdfFileFormat::FindByExtension("usda")) {
    return std::unexpected(Error(
        ErrorCode::kUnavailable,
        "OpenUSD: no plugin reads .usda files; its plugins (lib/usd) are not "
        "next to its libraries"));
  }
  UMBRA_LOG_DEBUG(kUsd, "{} plugins", registry.GetAllPlugins().size());
  return {};
}

// The messages of the errors posted since `mark`, joined: what OpenUSD
// says when a stage does not open.
std::string ErrorMessages(const pxr::TfErrorMark& mark) {
  std::string joined;
  for (const pxr::TfError& error : mark) {
    if (!joined.empty()) {
      joined += "; ";
    }
    joined += error.GetCommentary();
  }
  return joined.empty() ? std::string("unknown error") : joined;
}

// "cannot open '<path>': <reason>".
Error CannotOpen(ErrorCode code, std::string_view path,
                 std::string_view reason) {
  return {code, std::format("cannot open '{}': {}", path, reason)};
}

// Opens the stage of the file at `path`, with every payload loaded and no
// session layer. The errors of a stage that does not open become the
// returned error rather than log lines; those of a stage that opens are
// logged.
std::expected<pxr::UsdStageRefPtr, Error> OpenStage(const std::string& path) {
  // OpenUSD gives no reason for a file it cannot read; the system does.
  if (access(path.c_str(), R_OK) != 0) {
    const int reason = errno;
    return std::unexpected(CannotOpen(
        reason == ENOENT ? ErrorCode::kNotFound : ErrorCode::kInvalidArgument,
        path, std::generic_category().message(reason)));
  }
  const pxr::TfErrorMark mark;
  pxr::UsdStageRefPtr stage;
  try {
    if (const auto root = pxr::SdfLayer::FindOrOpen(path)) {
      stage = pxr::UsdStage::Open(root, pxr::SdfLayerHandle(),
                                  pxr::UsdStage::LoadAll);
    }
  } catch (const std::exception& error) {
    mark.Clear();
    return std::unexpected(
        CannotOpen(ErrorCode::kInvalidArgument, path, error.what()));
  }
  if (!stage) {
    const std::string reason = ErrorMessages(mark);
    mark.Clear();
    return std::unexpected(
        CannotOpen(ErrorCode::kInvalidArgument, path, reason));
  }
  return stage;
}

// The time to load: the frame given, else the stage's startTimeCode when it
// authors one, else the default time.
pxr::UsdTimeCode LoadTime(const pxr::UsdStage& stage,
                          std::optional<double> frame) {
  if (frame.has_value()) {
    return {*frame};
  }
  if (stage.HasAuthoredMetadata(pxr::SdfFieldKeys->StartTimeCode)) {
    return {stage.GetStartTimeCode()};
  }
  return pxr::UsdTimeCode::Default();
}

// "frame 12", "frame 12.5" or "default time".
std::string TimeName(pxr::UsdTimeCode time) {
  if (time.IsDefault()) {
    return "default time";
  }
  return std::format("frame {}", time.GetValue());
}

// The prims of the stage a render traverses: active, loaded, defined and
// not abstract.
std::size_t CountPrims(const pxr::UsdStageRefPtr& stage) {
  const pxr::UsdPrimRange prims = stage->Traverse();
  return static_cast<std::size_t>(std::distance(prims.begin(), prims.end()));
}

}  // namespace

std::expected<void, Error> Initialize() {
  RouteDiagnostics();
  // TF_DEBUG output goes to stdout by default, which carries only results;
  // Tf writes it to a file, so it cannot go through the log.
  pxr::TfDebug::SetOutputFile(stderr);
  try {
    if (auto loaded = LoadPlugins(); !loaded.has_value()) {
      return loaded;
    }
  } catch (const std::exception& error) {
    return std::unexpected(Error(
        ErrorCode::kUnavailable,
        std::format("OpenUSD: cannot load its plugins: {}", error.what())));
  }
  // OpenUSD runs on the process's oneTBB pool. umbra sets no limit of its
  // own; PXR_WORK_THREAD_LIMIT still applies.
  UMBRA_LOG_DEBUG(kUsd, "{} threads", pxr::WorkGetConcurrencyLimit());
  UMBRA_LOG_DEBUG(kUsd, "OpenUSD {}.{:02}", PXR_MINOR_VERSION,
                  PXR_PATCH_VERSION);
  return {};
}

std::expected<scene::Scene, Error> LoadScene(std::string_view path,
                                             const LoadOptions& options) {
  const auto start = std::chrono::steady_clock::now();
  // The number of layers is known once the stage is open: OpenUSD composes
  // it in one call.
  core::ProgressPhase phase("Loading stage", "layers");
  const auto stage = OpenStage(std::string(path));
  if (!stage.has_value()) {
    return std::unexpected(stage.error());
  }
  const pxr::SdfLayerHandleVector layers = (*stage)->GetUsedLayers();
  phase.SetTotal(layers.size());
  phase.Add(layers.size());
  const std::size_t prims = CountPrims(*stage);
  phase.Finish();
  for (const pxr::SdfLayerHandle& layer : layers) {
    UMBRA_LOG_TRACE(kUsd, "layer {}", layer->GetIdentifier());
  }
  const std::chrono::duration<double> elapsed =
      std::chrono::steady_clock::now() - start;
  UMBRA_LOG_DEBUG(kUsd, "stage opened in {} ({} layers, {} prims, {})",
                  core::FormatDuration(elapsed),
                  core::FormatCount(layers.size()), core::FormatCount(prims),
                  TimeName(LoadTime(**stage, options.frame)));
  return scene::Scene();
}

}  // namespace umbra::ext::usd
