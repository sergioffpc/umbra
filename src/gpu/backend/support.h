// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// What every part of the GPU module shares: its logging through umbra, and
// its errors, which are messages that the entry points turn into a
// ModuleStatus.

#ifndef UMBRA_GPU_BACKEND_SUPPORT_H_
#define UMBRA_GPU_BACKEND_SUPPORT_H_

#include <driver_types.h>
#include <optix_types.h>

#include <atomic>
#include <concepts>
#include <expected>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "umbra/core/log.h"
#include "umbra/core/safety.h"
#include "umbra/gpu/module_abi.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::gpu::backend {

// umbra's log function and the gpu component's threshold, handed over by the
// loader before any other call (ModuleApi::set_log).
struct LogState {
  std::atomic<ModuleLogFunction> log{nullptr};
  std::atomic<core::LogLevel> threshold{core::LogLevel::kError};
};

// The module's one LogState.
LogState& Logging();

// A format string and the source line it is written on.
template <typename... Args>
struct LocatedFormat {
  template <typename Text>
    requires std::convertible_to<const Text&, std::string_view>
  // A string literal converts to it, as to std::format_string.
  // NOLINTBEGIN(cppcoreguidelines-explicit-constructor): see above.
  // NOLINTBEGIN(misc-explicit-constructor): see above.
  // NOLINTBEGIN(google-explicit-constructor): see above.
  consteval LocatedFormat(const Text& text, std::source_location location =
                                                std::source_location::current())
      : format(text), location(location) {}
  // NOLINTEND(google-explicit-constructor)
  // NOLINTEND(misc-explicit-constructor)
  // NOLINTEND(cppcoreguidelines-explicit-constructor)

  std::format_string<Args...> format;
  std::source_location location;
};

// Logs a message through umbra, formatting it only when it will be shown.
template <typename... Args>
void Log(core::LogLevel level,
         LocatedFormat<std::type_identity_t<Args>...> format, Args&&... args) {
  const ModuleLogFunction log = Logging().log.load(std::memory_order_relaxed);
  if (log == nullptr ||
      level > Logging().threshold.load(std::memory_order_relaxed)) {
    return;
  }
  log(level, std::format(format.format, std::forward<Args>(args)...),
      {.file = format.location.file_name(), .line = format.location.line()});
}

// The oldest GPU architecture the module is built for: sm_120.
inline constexpr int kMinComputeCapabilityMajor = 12;

// Errors inside the module are messages; the entry points turn them into a
// ModuleStatus.
template <typename T>
using Result = std::expected<T, std::string>;

// Fails with the CUDA runtime's message when `error` is not cudaSuccess.
Result<void> CheckCuda(cudaError_t error, std::string_view what);

// Fails with OptiX's message when `result` is not OPTIX_SUCCESS.
Result<void> CheckOptix(OptixResult result, std::string_view what);

// Loads the driver's OptiX runtime, once per process.
Result<void> InitOptix();

}  // namespace umbra::gpu::backend

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_GPU_BACKEND_SUPPORT_H_
