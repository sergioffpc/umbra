// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Wrapping (modular) integer arithmetic: the only way to overflow on purpose,
// for code such as random number generators and hashes. Ordinary arithmetic
// that overflows aborts the sanitizer test builds; these helpers are exempt
// and state the intent at the call site.

#ifndef UMBRA_CORE_WRAPPING_H_
#define UMBRA_CORE_WRAPPING_H_

#include <concepts>
#include <limits>
#include <type_traits>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

// Exempts a function from the sanitizer checks of intentional overflow.
#define UMBRA_INTERNAL_NO_SANITIZE_WRAPPING \
  __attribute__((                           \
      no_sanitize("unsigned-integer-overflow", "unsigned-shift-base")))

namespace umbra::core {

// The integer types the wrapping helpers accept: every integral type but
// bool.
template <typename T>
concept WrappingInteger = std::integral<T> && !std::same_as<T, bool>;

namespace internal_wrapping {

// The unsigned type the arithmetic of T is computed in: never narrower than
// unsigned int, so that integral promotion cannot turn it into signed int
// arithmetic, which must not overflow.
template <WrappingInteger T>
using Unsigned = std::common_type_t<std::make_unsigned_t<T>, unsigned>;

}  // namespace internal_wrapping

// Returns a + b modulo 2^N, where N is the width of T.
template <WrappingInteger T>
UMBRA_INTERNAL_NO_SANITIZE_WRAPPING constexpr T WrappingAdd(T a, T b) {
  using U = internal_wrapping::Unsigned<T>;
  return static_cast<T>(static_cast<U>(a) + static_cast<U>(b));
}

// Returns a - b modulo 2^N, where N is the width of T.
template <WrappingInteger T>
UMBRA_INTERNAL_NO_SANITIZE_WRAPPING constexpr T WrappingSub(T a, T b) {
  using U = internal_wrapping::Unsigned<T>;
  return static_cast<T>(static_cast<U>(a) - static_cast<U>(b));
}

// Returns a * b modulo 2^N, where N is the width of T.
template <WrappingInteger T>
UMBRA_INTERNAL_NO_SANITIZE_WRAPPING constexpr T WrappingMul(T a, T b) {
  using U = internal_wrapping::Unsigned<T>;
  return static_cast<T>(static_cast<U>(a) * static_cast<U>(b));
}

// Returns a shifted left by (shift modulo N) bits, where N is the width of T;
// the bits shifted out are discarded.
template <WrappingInteger T>
UMBRA_INTERNAL_NO_SANITIZE_WRAPPING constexpr T WrappingShl(T a,
                                                            unsigned shift) {
  using U = internal_wrapping::Unsigned<T>;
  constexpr unsigned kMask =
      std::numeric_limits<std::make_unsigned_t<T>>::digits - 1U;
  return static_cast<T>(static_cast<U>(a) << (shift & kMask));
}

}  // namespace umbra::core

#undef UMBRA_INTERNAL_NO_SANITIZE_WRAPPING

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_CORE_WRAPPING_H_
