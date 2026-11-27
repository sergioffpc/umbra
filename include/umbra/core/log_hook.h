// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// A log that goes through a function handed over at run time. Header-only:
// the GPU module, which links no umbra module, and the bridges linked into
// it log through umbra's diagnostics this way.

#ifndef UMBRA_CORE_LOG_HOOK_H_
#define UMBRA_CORE_LOG_HOOK_H_

#include <atomic>
#include <source_location>
#include <string_view>

#include "umbra/core/log.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::core {

// A function that logs `message` at `level`, from the source line `origin`
// names.
using LogFunction = void (*)(LogLevel level, std::string_view message,
                             LogOrigin origin);

// The function a log goes through, and the least severe level it shows.
// Nothing is logged while the function is null. Any thread may log through
// it, and set it.
class LogHook {
 public:
  // Logs through `log` from now on, the messages of `threshold` and more
  // severe ones.
  void Set(LogFunction UMBRA_NULLABLE log, LogLevel threshold) {
    threshold_.store(threshold, std::memory_order_relaxed);
    log_.store(log, std::memory_order_relaxed);
  }

  // The function that logs a message of `level`; null when it is not shown,
  // so that the message need not be made.
  [[nodiscard]] LogFunction UMBRA_NULLABLE For(LogLevel level) const {
    if (level > threshold_.load(std::memory_order_relaxed)) {
      return nullptr;
    }
    return log_.load(std::memory_order_relaxed);
  }

  // Logs `message` at `level`, from the caller's line, when it is shown.
  void Log(
      LogLevel level, std::string_view message,
      std::source_location location = std::source_location::current()) const {
    if (const LogFunction UMBRA_NULLABLE log = For(level); log != nullptr) {
      log(level, message,
          {.file = location.file_name(), .line = location.line()});
    }
  }

 private:
  std::atomic<LogFunction> log_{nullptr};
  std::atomic<LogLevel> threshold_{LogLevel::kError};
};

}  // namespace umbra::core

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_CORE_LOG_HOOK_H_
