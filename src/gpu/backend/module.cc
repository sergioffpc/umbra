// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The GPU module, libumbra-gpu.so: the entry point umbra loads (see
// include/umbra/gpu/module_abi.h) over the CUDA runtime and OptiX. For now it
// finds the usable GPUs; the GPU executors will live here.

#include <cuda_runtime_api.h>
#include <driver_types.h>
#include <optix.h>
#include <optix_function_table.h>
#include <optix_function_table_definition.h>  // IWYU pragma: keep
#include <optix_stubs.h>
#include <optix_types.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "umbra/core/log.h"
#include "umbra/gpu/module_abi.h"

namespace umbra::gpu::backend {
namespace {

// The oldest GPU architecture the module is built for: sm_120.
constexpr int kMinComputeCapabilityMajor = 12;

// umbra's log function and the gpu component's threshold, handed over by the
// loader before any other call (ModuleApi::set_log).
struct LogState {
  std::atomic<ModuleLogFunction> log{nullptr};
  std::atomic<core::LogLevel> threshold{core::LogLevel::kError};
};

LogState& Logging() {
  static LogState state;
  return state;
}

void SetLog(ModuleLogFunction log, core::LogLevel threshold) {
  Logging().log.store(log, std::memory_order_relaxed);
  Logging().threshold.store(threshold, std::memory_order_relaxed);
}

// Logs a message through umbra, formatting it only when it will be shown.
template <typename... Args>
void Log(core::LogLevel level, std::format_string<Args...> format,
         Args&&... args) {
  const ModuleLogFunction log = Logging().log.load(std::memory_order_relaxed);
  if (log == nullptr ||
      level > Logging().threshold.load(std::memory_order_relaxed)) {
    return;
  }
  log(level, std::format(format, std::forward<Args>(args)...));
}

// Errors inside the module are messages; the entry points turn them into a
// ModuleStatus.
template <typename T>
using Result = std::expected<T, std::string>;

Result<void> CheckCuda(cudaError_t error, std::string_view what) {
  if (error == cudaSuccess) {
    return {};
  }
  return std::unexpected(std::format("CUDA: {}: {} ({})", what,
                                     cudaGetErrorString(error),
                                     cudaGetErrorName(error)));
}

// Loads the driver's OptiX runtime, once per process.
Result<void> InitOptix() {
  static const OptixResult kResult = optixInit();
  switch (kResult) {
    case OPTIX_SUCCESS:
      return {};
    case OPTIX_ERROR_LIBRARY_NOT_FOUND:
      return std::unexpected(std::string(
          "OptiX: the driver's OptiX runtime (libnvoptix.so.1) is missing; "
          "in a container, the GPU must come with the graphics libraries"));
    case OPTIX_ERROR_UNSUPPORTED_ABI_VERSION:
      return std::unexpected(std::format(
          "OptiX: the driver is too old for OptiX {}.{}.{} (ABI {})",
          OPTIX_VERSION / 10000, OPTIX_VERSION / 100 % 100, OPTIX_VERSION % 100,
          OPTIX_ABI_VERSION));
    default:
      return std::unexpected(std::format("OptiX: optixInit failed ({})",
                                         static_cast<int>(kResult)));
  }
}

// Whether the GPU at `ordinal`, of compute capability `major`, is one the
// module runs on; logs it either way.
bool IsUsable(int ordinal, int major) {
  cudaDeviceProp properties{};
  const std::string name =
      cudaGetDeviceProperties(&properties, ordinal) == cudaSuccess
          ? std::string(std::ranges::begin(properties.name),
                        std::ranges::find(properties.name, '\0'))
          : std::string("unknown");
  if (major < kMinComputeCapabilityMajor) {
    Log(core::LogLevel::kDebug,
        "device {}: {} (sm_{}{}), skipped: older than sm_{}0", ordinal, name,
        properties.major, properties.minor, kMinComputeCapabilityMajor);
    return false;
  }
  Log(core::LogLevel::kInfo, "device {}: {} (sm_{}{})", ordinal, name,
      properties.major, properties.minor);
  return true;
}

// Logs the versions of the driver and OptiX the module runs on.
void LogVersions() {
  int driver = 0;
  if (cudaDriverGetVersion(&driver) != cudaSuccess) {
    driver = 0;
  }
  Log(core::LogLevel::kInfo, "CUDA driver {}.{}, OptiX {}.{}.{}", driver / 1000,
      driver % 1000 / 10, OPTIX_VERSION / 10000, OPTIX_VERSION / 100 % 100,
      OPTIX_VERSION % 100);
}

// The CUDA ordinals of the usable GPUs, in CUDA order.
Result<std::vector<int>> UsableDevices() {
  int count = 0;
  if (auto checked = CheckCuda(cudaGetDeviceCount(&count), "no device");
      !checked) {
    return std::unexpected(checked.error());
  }
  std::vector<int> usable;
  for (int ordinal = 0; ordinal < count; ++ordinal) {
    int major = 0;
    if (auto checked =
            CheckCuda(cudaDeviceGetAttribute(
                          &major, cudaDevAttrComputeCapabilityMajor, ordinal),
                      "compute capability");
        !checked) {
      return std::unexpected(checked.error());
    }
    if (IsUsable(ordinal, major)) {
      usable.push_back(ordinal);
    }
  }
  if (usable.empty()) {
    return usable;
  }
  return InitOptix().transform([&] {
    LogVersions();
    return usable;
  });
}

ModuleStatus CountDevices(std::int32_t* count) {
  auto devices = UsableDevices();
  if (devices) {
    *count = static_cast<std::int32_t>(devices->size());
    return {};
  }
  return FailedStatus(devices.error());
}

constexpr ModuleApi kApi{
    .abi_version = kModuleAbiVersion,
    .set_log = SetLog,
    .count_devices = CountDevices,
};

}  // namespace
}  // namespace umbra::gpu::backend

extern "C" [[gnu::visibility("default")]] const umbra::gpu::ModuleApi*
umbra_gpu_module() {  // NOLINT(readability-identifier-naming): C symbol.
  return &umbra::gpu::backend::kApi;
}
