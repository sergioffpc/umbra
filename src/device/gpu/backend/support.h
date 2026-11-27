// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// What every part of the GPU module shares: its logging through umbra, and
// its errors, which are messages that the entry points turn into a
// ModuleStatus.

#ifndef UMBRA_DEVICE_GPU_BACKEND_SUPPORT_H_
#define UMBRA_DEVICE_GPU_BACKEND_SUPPORT_H_

#include <concepts>
#include <expected>
#include <format>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "umbra/core/log.h"
#include "umbra/core/log_hook.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device::gpu::backend {

// The module's log: umbra's log function and the gpu component's threshold,
// handed over by the loader before any other call (ModuleApi::set_log).
core::LogHook& Logging();

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
  if (const core::LogFunction UMBRA_NULLABLE log = Logging().For(level);
      log != nullptr) {
    log(level, std::format(format.format, std::forward<Args>(args)...),
        {.file = format.location.file_name(), .line = format.location.line()});
  }
}

// The oldest GPU architecture the module is built for: sm_120.
inline constexpr int kMinComputeCapabilityMajor = 12;

// Errors inside the module are messages, as those of the bridges to CUDA and
// OptiX are; the entry points turn them into a ModuleStatus.
template <typename T>
using Result = std::expected<T, std::string>;

}  // namespace umbra::device::gpu::backend

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_GPU_BACKEND_SUPPORT_H_
