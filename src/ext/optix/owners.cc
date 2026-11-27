// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/optix/owners.h"

#include <optix_host.h>
#include <optix_stubs.h>
#include <optix_types.h>

#include <exception>
#include <format>
#include <string_view>

#include "log.h"
#include "umbra/core/handle_owner.h"
#include "umbra/core/log.h"
#include "umbra/core/log_hook.h"
#include "umbra/core/safety.h"
#include "umbra/ext/optix/errors.h"

namespace umbra::ext::optix {
namespace {

// The most verbose of OptiX's levels that are warnings: 1 fatal, 2 error,
// 3 warning.
constexpr unsigned kWarningLevel = 3;

// OptiX's warnings and errors, as warnings, and the rest at debug level.
void LogFromOptix(unsigned level, const char* UMBRA_NULLABLE tag,
                  const char* UMBRA_NULLABLE message,
                  void* UMBRA_NULLABLE /*data*/) {
  const core::LogLevel umbra_level = level <= kWarningLevel
                                         ? core::LogLevel::kWarning
                                         : core::LogLevel::kDebug;
  try {
    Logging().Log(umbra_level,
                  std::format("OptiX: {}: {}", tag == nullptr ? "" : tag,
                              message == nullptr ? "" : message));
  } catch (const std::exception& error) {
    Logging().Log(umbra_level, error.what());
  }
}

}  // namespace

void DeviceContext::DestroyLogged(OptixDeviceContext context) {
  try {
    if (auto destroyed = Check(optixDeviceContextDestroy(context),
                               "cannot destroy a context");
        !destroyed.has_value()) {
      Logging().Log(core::LogLevel::kWarning, destroyed.error());
    }
  } catch (const std::exception& error) {
    Logging().Log(core::LogLevel::kWarning, error.what());
  }
}

Result<DeviceContext> DeviceContext::Create(std::string_view what) {
  OptixDeviceContextOptions options{};
  options.logCallbackFunction = LogFromOptix;
  options.logCallbackLevel = kWarningLevel;
  OptixDeviceContext context = nullptr;
  // The current CUDA context, which the runtime made for the current GPU.
  return Check(optixDeviceContextCreate(nullptr, &options, &context), what)
      .transform([&] { return DeviceContext(context); });
}

Result<void> DeviceContext::Destroy(std::string_view what) {
  if (context_.get() == nullptr) {
    return {};
  }
  return Check(optixDeviceContextDestroy(context_.release()), what);
}

void DestroyModule(OptixModule module) {
  static_cast<void>(optixModuleDestroy(module));
}

void DestroyProgramGroup(OptixProgramGroup group) {
  static_cast<void>(optixProgramGroupDestroy(group));
}

void DestroyPipeline(OptixPipeline pipeline) {
  static_cast<void>(optixPipelineDestroy(pipeline));
}

}  // namespace umbra::ext::optix
