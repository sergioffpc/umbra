// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/image.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace umbra::device {
namespace {

constexpr std::array<std::int32_t, 2> kResolution{8, 4};

TEST(PixelWindowOfTest, IncludesEveryPixelOfTheFullWindow) {
  const PixelWindow window =
      PixelWindowOf(kResolution, {0.0F, 0.0F, 1.0F, 1.0F});

  EXPECT_EQ(window,
            (PixelWindow{.x_min = 0, .y_min = 0, .x_end = 8, .y_end = 4}));
}

TEST(PixelWindowOfTest, CropsToThePixelsWhoseCentersLieInTheWindow) {
  // Centers at x = 0.0625 + 0.125 i and y = 0.875 - 0.25 j, from the top.
  // The left half, and the bottom half with y flipped: rows 2 and 3.
  const PixelWindow window =
      PixelWindowOf(kResolution, {0.0F, 0.0F, 0.5F, 0.5F});

  EXPECT_EQ(window,
            (PixelWindow{.x_min = 0, .y_min = 2, .x_end = 4, .y_end = 4}));
}

TEST(PixelWindowOfTest, IncludesAPixelWhoseCenterIsOnTheEdge) {
  // 0.1875 is the center of column 1, 0.375 that of row 2 from the bottom,
  // row 1 from the top.
  const PixelWindow window =
      PixelWindowOf(kResolution, {0.1875F, 0.375F, 0.6875F, 0.625F});

  EXPECT_EQ(window,
            (PixelWindow{.x_min = 1, .y_min = 1, .x_end = 6, .y_end = 3}));
}

TEST(PixelWindowOfTest, OverscansBeyondTheResolution) {
  // A quarter of the resolution more on each side: 2 columns and 1 row.
  const PixelWindow window =
      PixelWindowOf(kResolution, {-0.25F, -0.25F, 1.25F, 1.25F});

  EXPECT_EQ(window,
            (PixelWindow{.x_min = -2, .y_min = -1, .x_end = 10, .y_end = 5}));
}

TEST(PixelWindowOfTest, IsEmptyWhenNoCenterLiesInTheWindow) {
  // Between the centers of columns 0 and 1.
  const PixelWindow window =
      PixelWindowOf(kResolution, {0.07F, 0.0F, 0.18F, 1.0F});

  EXPECT_TRUE(window.IsEmpty());
}

TEST(PixelWindowOfTest, IsEmptyForAnInvertedWindow) {
  EXPECT_TRUE(PixelWindowOf(kResolution, {0.75F, 0.0F, 0.25F, 1.0F}).IsEmpty());
}

TEST(PixelCenterNdcTest, FlipsYFromTheTop) {
  const std::array<double, 2> top_left = PixelCenterNdc(kResolution, 0, 0);
  const std::array<double, 2> bottom_right = PixelCenterNdc(kResolution, 7, 3);

  EXPECT_DOUBLE_EQ(top_left.at(0), 0.0625);
  EXPECT_DOUBLE_EQ(top_left.at(1), 0.875);
  EXPECT_DOUBLE_EQ(bottom_right.at(0), 0.9375);
  EXPECT_DOUBLE_EQ(bottom_right.at(1), 0.125);
}

}  // namespace
}  // namespace umbra::device
