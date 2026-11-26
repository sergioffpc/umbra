// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/device.h"

#include <cstddef>

#include "umbra/device/render_data.h"

namespace umbra::device {

std::size_t Primvar::Count() const {
  const std::size_t numbers = floats.size() + integers.size();
  return components == 0 ? 0 : numbers / components;
}

Device::~Device() = default;

}  // namespace umbra::device
