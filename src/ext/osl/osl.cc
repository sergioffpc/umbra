// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/osl/osl.h"

#include <OSL/oslconfig.h>
#include <OSL/oslexec.h>
#include <OSL/oslversion.h>
#include <OSL/rendererservices.h>
// The definition of ErrorHandler, which LogErrorHandler derives from; the
// name reaches OIIO:: through nsversions.h.
#include <OpenImageIO/errorhandler.h>  // IWYU pragma: keep
#include <OpenImageIO/nsversions.h>

#include <algorithm>
#include <array>
#include <exception>
#include <expected>
#include <string>

#include "umbra/core/error.h"
#include "umbra/core/log.h"

namespace umbra::ext::osl {
namespace {

using ::umbra::core::LogComponent;
using ::umbra::core::LogLevel;

// umbra's level for an OpenImageIO error code, which OSL's shading systems
// report with.
LogLevel LevelOf(int code) {
  switch (code) {
    case OIIO::ErrorHandler::EH_ERROR:
    case OIIO::ErrorHandler::EH_SEVERE:
      return LogLevel::kError;
    case OIIO::ErrorHandler::EH_WARNING:
      return LogLevel::kWarning;
    case OIIO::ErrorHandler::EH_DEBUG:
      return LogLevel::kDebug;
    default:
      return LogLevel::kInfo;
  }
}

// The error handler of the process's shading systems: every message goes to
// umbra's log, which filters it by level.
class LogErrorHandler final : public OIIO::ErrorHandler {
 public:
  void operator()(int code, const std::string& message) override {
    core::Log(LevelOf(code), LogComponent::kOsl, message);
  }
};

// The error handler of every shading system of the process, which it
// outlives.
LogErrorHandler& ProcessHandler() {
  static LogErrorHandler handler;
  return handler;
}

// The batch widths umbra's OSL is built for, widest first.
constexpr std::array kBatchWidths = {16, 8};

#if OSL_USE_BATCHED
// The widest of kBatchWidths `shading` can be configured for, or 0.
int WidestBatch(OSL::ShadingSystem& shading) {
  const auto* const width =
      std::ranges::find_if(kBatchWidths, [&shading](int candidate) {
        return shading.configure_batch_execution_at(candidate);
      });
  return width == kBatchWidths.end() ? 0 : *width;
}
#endif

int MeasureBatchWidth() {
#if OSL_USE_BATCHED
  try {
    // OSL needs a renderer; the base one does for configuring a batch.
    OSL::RendererServices renderer;
    OSL::ShadingSystem shading(&renderer, nullptr, &ProcessHandler());
    // Batched execution needs fused multiply-add to be allowed.
    shading.attribute("llvm_jit_fma", 1);
    return WidestBatch(shading);
  } catch (const std::exception& error) {
    UMBRA_LOG_WARNING(kOsl, "cannot configure batched shading: {}",
                      error.what());
  }
#endif
  return 0;
}

}  // namespace

int BatchWidth() {
  static const int kWidth = MeasureBatchWidth();
  return kWidth;
}

std::expected<void, core::Error> Initialize() {
  LogErrorHandler& handler = ProcessHandler();
  handler.verbosity(core::IsLogEnabled(LogComponent::kOsl, LogLevel::kDebug)
                        ? OIIO::ErrorHandler::VERBOSE
                        : OIIO::ErrorHandler::NORMAL);
  UMBRA_LOG_INFO(kOsl, "OSL {}", OSL_LIBRARY_VERSION_STRING);
  return {};
}

}  // namespace umbra::ext::osl
