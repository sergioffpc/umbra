// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#ifndef UMBRA_CORE_ERROR_H_
#define UMBRA_CORE_ERROR_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::core {

// The category of an error, for callers that react to it programmatically.
enum class ErrorCode : std::uint8_t {
  // The caller passed a value outside the function's contract.
  kInvalidArgument,
  // A requested entity (file, asset, key) does not exist.
  kNotFound,
  // A value or index is outside the representable or valid range.
  kOutOfRange,
  // The operating system or a library failed to perform an operation.
  kUnavailable,
  // An invariant of umbra itself was broken.
  kInternal,
};

// The error of every fallible umbra operation, returned as
// std::expected<T, Error>. Exceptions are disabled: errors are values, and
// discarding one is an error.
class [[nodiscard]] Error {
 public:
  Error(ErrorCode code, std::string message);

  ErrorCode code() const { return code_; }
  std::string_view message() const UMBRA_LIFETIMEBOUND { return message_; }

 private:
  ErrorCode code_;
  std::string message_;
};

}  // namespace umbra::core

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_CORE_ERROR_H_
