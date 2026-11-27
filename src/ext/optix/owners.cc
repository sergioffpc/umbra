// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/optix/owners.h"

#include <optix_host.h>
#include <optix_stubs.h>
#include <optix_types.h>

#include <exception>
#include <format>
#include <string_view>
#include <utility>

#include "log.h"
#include "umbra/core/log.h"
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
    Log(umbra_level, std::format("OptiX: {}: {}", tag == nullptr ? "" : tag,
                                 message == nullptr ? "" : message));
  } catch (const std::exception& error) {
    Log(umbra_level, error.what());
  }
}

}  // namespace

DeviceContext::~DeviceContext() {
  try {
    if (auto destroyed = Destroy("cannot destroy a context");
        !destroyed.has_value()) {
      Log(core::LogLevel::kWarning, destroyed.error());
    }
  } catch (const std::exception& error) {
    Log(core::LogLevel::kWarning, error.what());
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
  if (context_ == nullptr) {
    return {};
  }
  return Check(optixDeviceContextDestroy(std::exchange(context_, nullptr)),
               what);
}

void ModuleDestroyer::operator()(OptixModule module) const {
  static_cast<void>(optixModuleDestroy(module));
}

void ProgramGroupDestroyer::operator()(OptixProgramGroup group) const {
  static_cast<void>(optixProgramGroupDestroy(group));
}

void PipelineDestroyer::operator()(OptixPipeline pipeline) const {
  static_cast<void>(optixPipelineDestroy(pipeline));
}

}  // namespace umbra::ext::optix
