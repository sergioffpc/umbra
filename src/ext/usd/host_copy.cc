// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "host_copy.h"

#include <cstddef>
#include <variant>

namespace umbra::ext::usd::internal::host {

std::size_t Primvar::Count() const {
  const std::size_t numbers =
      std::visit([](const auto& numbers) { return numbers.size(); }, values);
  return components == 0 ? 0 : numbers / components;
}

}  // namespace umbra::ext::usd::internal::host
