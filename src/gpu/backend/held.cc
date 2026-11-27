// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "held.h"

#include <driver_types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "buffer.h"
#include "support.h"
#include "umbra/device/render_data.h"

namespace umbra::gpu::backend {

Result<HeldPrimvar> UploadPrimvar(const device::Primvar& primvar,
                                  cudaStream_t stream) {
  HeldPrimvar held{
      .name = std::string(primvar.name),
      .rate = primvar.rate,
      .components = primvar.components,
      .floats = {},
      .integers = {},
      .indices = {},
  };
  return Replace(held.floats, primvar.floats, stream)
      .and_then(
          [&] { return Replace(held.integers, primvar.integers, stream); })
      .and_then([&] { return Replace(held.indices, primvar.indices, stream); })
      .transform([&] { return std::move(held); });
}

std::array<float, 12> RowsOf(const device::Matrix& matrix) {
  std::array<float, 12> rows{};
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t column = 0; column < 4; ++column) {
      rows.at((row * 4) + column) =
          static_cast<float>(matrix.at((column * 4) + row));
    }
  }
  return rows;
}

Result<HeldPlacements> UploadPlacements(
    std::span<const device::Instance> instances, cudaStream_t stream) {
  std::vector<KeyRange> ranges;
  ranges.reserve(instances.size());
  std::vector<std::array<float, 12>> keys;
  for (const device::Instance& instance : instances) {
    ranges.push_back({
        .first = static_cast<std::uint32_t>(keys.size()),
        .count = static_cast<std::uint32_t>(instance.object_to_world.size()),
    });
    for (const device::Matrix& matrix : instance.object_to_world) {
      keys.push_back(RowsOf(matrix));
    }
  }
  HeldPlacements held{
      .ranges = {},
      .keys = {},
      .count = static_cast<std::uint32_t>(instances.size()),
  };
  return Replace(held.ranges, std::span<const KeyRange>(ranges), stream)
      .and_then([&] {
        return Replace(held.keys, std::span<const std::array<float, 12>>(keys),
                       stream);
      })
      .transform([&] { return std::move(held); });
}

}  // namespace umbra::gpu::backend
