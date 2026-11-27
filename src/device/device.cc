// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/device.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "umbra/core/progress.h"
#include "umbra/core/safety.h"
#include "umbra/device/image.h"
#include "umbra/device/render_data.h"

namespace umbra::device {

std::size_t Primvar::Count() const {
  const std::size_t numbers = floats.size() + integers.size();
  return components == 0 ? 0 : numbers / components;
}

namespace {

// The tiles along a side of `pixels` pixels.
std::uint64_t TilesAlong(std::int32_t pixels) {
  return pixels <= 0
             ? 0
             : (static_cast<std::uint64_t>(pixels) + kTileSize - 1) / kTileSize;
}

}  // namespace

TileQueue::TileQueue(const PixelWindow& window,
                     core::ProgressPhase* UMBRA_NULLABLE progress)
    : window_(window),
      columns_(TilesAlong(window.width())),
      count_(columns_ * TilesAlong(window.height())),
      progress_(progress) {}

std::uint64_t TileQueue::taken() const {
  return std::min(next_.load(std::memory_order_relaxed), count_);
}

PixelWindow TileQueue::TileWindow(std::uint64_t tile) const {
  const auto column = static_cast<std::int32_t>(tile % columns_);
  const auto row = static_cast<std::int32_t>(tile / columns_);
  const std::int32_t x_min = window_.x_min + (column * kTileSize);
  const std::int32_t y_min = window_.y_min + (row * kTileSize);
  return {
      .x_min = x_min,
      .y_min = y_min,
      .x_end = std::min(x_min + kTileSize, window_.x_end),
      .y_end = std::min(y_min + kTileSize, window_.y_end),
  };
}

Device::~Device() = default;

}  // namespace umbra::device
