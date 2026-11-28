// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/device.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

#include "umbra/device/image.h"

namespace umbra::device {
namespace {

using Batches = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

// The tiles of a window three tiles wide and one high.
constexpr PixelWindow kThreeTiles{
    .x_min = 0,
    .y_min = 0,
    .x_end = 3 * kTileSize,
    .y_end = kTileSize,
};

TEST(DeviceNameTest, NamesACpuDeviceByItsNumaNode) {
  EXPECT_EQ(DeviceName(DeviceKind::kCpu, 1), "cpu1");
}

TEST(DeviceNameTest, NamesAGpuDeviceByItsOrdinal) {
  EXPECT_EQ(DeviceName(DeviceKind::kGpu, 0), "gpu0");
}

TEST(TileQueueTest, RendersEachBatchUntilNoTileIsLeft) {
  TileQueue tiles(kThreeTiles);
  Batches batches;

  const std::expected<void, std::string> rendered =
      tiles.ForEachBatch(2, [&](TileQueue::Batch batch) {
        batches.emplace_back(batch.first, batch.end);
        return std::expected<void, std::string>();
      });

  EXPECT_TRUE(rendered.has_value());
  EXPECT_EQ(batches, (Batches{{0, 2}, {2, 3}}));
  EXPECT_EQ(tiles.taken(), 3U);
}

TEST(TileQueueTest, StopsAtTheFirstBatchItFailsToRender) {
  TileQueue tiles(kThreeTiles);
  Batches batches;

  const std::expected<void, std::string> rendered =
      tiles.ForEachBatch(1, [&](TileQueue::Batch batch) {
        batches.emplace_back(batch.first, batch.end);
        return batch.first == 1 ? std::expected<void, std::string>(
                                      std::unexpected("tile 1 failed"))
                                : std::expected<void, std::string>();
      });

  ASSERT_FALSE(rendered.has_value());
  EXPECT_EQ(rendered.error(), "tile 1 failed");
  EXPECT_EQ(batches, (Batches{{0, 1}, {1, 2}}));
}

}  // namespace
}  // namespace umbra::device
