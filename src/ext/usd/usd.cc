// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/usd/usd.h"

#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/callContext.h>
#include <pxr/base/tf/debug.h>
#include <pxr/base/tf/diagnosticLite.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/error.h>
#include <pxr/base/tf/status.h>
#include <pxr/base/tf/warning.h>
#include <pxr/base/work/threadLimits.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/fileFormat.h>

#include <exception>
#include <expected>
#include <format>
#include <string>

#include "umbra/core/error.h"
#include "umbra/core/log.h"

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
    core::Log(
        LogLevel::kError, LogComponent::kUsd,
        std::format("fatal error in {}: {}", context.GetFunction(), message));
  }

  void IssueStatus(const pxr::TfStatus& status) override {
    if (!status.GetQuiet()) {
      core::Log(LogLevel::kInfo, LogComponent::kUsd, status.GetCommentary());
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
  UMBRA_LOG_INFO(kUsd, "OpenUSD {}.{:02}", PXR_MINOR_VERSION,
                 PXR_PATCH_VERSION);
  return {};
}

}  // namespace umbra::ext::usd
