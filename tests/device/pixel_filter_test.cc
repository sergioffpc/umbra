// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/pixel_filter.h"

#include <gtest/gtest.h>

namespace umbra::device {
namespace {

// The integral of the filter's density from -0.75 to `offset`, by the
// midpoint rule.
double Integral(float offset) {
  constexpr int kSteps = 100000;
  const double low = -0.5 * static_cast<double>(kPixelFilterWidth);
  const double step = (static_cast<double>(offset) - low) / kSteps;
  double sum = 0.0;
  for (int index = 0; index < kSteps; ++index) {
    const double point = low + ((index + 0.5) * step);
    sum += static_cast<double>(PixelFilterDensity(static_cast<float>(point)));
  }
  return sum * step;
}

TEST(PixelFilterTest, IsOnePointFiveWide) {
  EXPECT_FLOAT_EQ(kPixelFilterWidth, 1.5F);
}

TEST(PixelFilterDensityTest, IntegratesToOne) {
  EXPECT_NEAR(Integral(0.75F), 1.0, 1e-6);
}

TEST(PixelFilterDensityTest, PeaksAtTheCenterAsBlackmanHarris) {
  // Blackman-Harris is 1 at its center and integrates to a0 = 0.35875
  // over its width.
  EXPECT_NEAR(PixelFilterDensity(0.0F), 1.0 / (0.35875 * 1.5), 1e-5);
}

TEST(PixelFilterDensityTest, IsZeroBeyondItsWidth) {
  EXPECT_EQ(PixelFilterDensity(0.76F), 0.0F);
  EXPECT_EQ(PixelFilterDensity(-0.76F), 0.0F);
  EXPECT_EQ(PixelFilterDensity(3.0F), 0.0F);
}

TEST(PixelFilterDensityTest, IsSymmetric) {
  EXPECT_FLOAT_EQ(PixelFilterDensity(0.3F), PixelFilterDensity(-0.3F));
}

TEST(SamplePixelFilterTest, KeepsToItsWidth) {
  for (const float uniform : {0.0F, 1e-7F, 0.5F, 0.9999999F, 1.0F}) {
    const float offset = SamplePixelFilter(uniform);
    EXPECT_GE(offset, -0.75F) << uniform;
    EXPECT_LE(offset, 0.75F) << uniform;
  }
}

TEST(SamplePixelFilterTest, MapsTheMiddleToTheCenter) {
  EXPECT_NEAR(SamplePixelFilter(0.5F), 0.0F, 1e-6F);
}

TEST(SamplePixelFilterTest, FollowsTheDensity) {
  // A sample at u has a fraction u of the density below it.
  for (const float uniform : {0.01F, 0.1F, 0.25F, 0.4F, 0.6F, 0.75F, 0.99F}) {
    EXPECT_NEAR(Integral(SamplePixelFilter(uniform)),
                static_cast<double>(uniform), 1e-5)
        << uniform;
  }
}

TEST(SamplePixelFilterTest, SamplesEachAxisApart) {
  const FilterOffset offset = SamplePixelFilter(0.25F, 0.75F);

  EXPECT_FLOAT_EQ(offset.x, SamplePixelFilter(0.25F));
  EXPECT_FLOAT_EQ(offset.y, SamplePixelFilter(0.75F));
}

}  // namespace
}  // namespace umbra::device
