// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The words of a count in the messages the render delegate logs.

#ifndef UMBRA_DELEGATE_PLURAL_H_
#define UMBRA_DELEGATE_PLURAL_H_

#include <cstddef>
#include <string_view>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

// `singular` for one, `plural` otherwise.
[[nodiscard]] constexpr std::string_view Plural(std::size_t count,
                                                std::string_view singular,
                                                std::string_view plural) {
  return count == 1 ? singular : plural;
}

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_PLURAL_H_
