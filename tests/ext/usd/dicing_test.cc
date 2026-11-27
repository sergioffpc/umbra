// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "dicing.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

#include "umbra/device/render_data.h"

namespace umbra::ext::usd::internal::subdivision {
namespace {

// A transform that moves by `offset`, in meters.
device::Matrix Translation(double offset_x, double offset_y, double offset_z) {
  device::Matrix matrix{};
  for (std::size_t axis = 0; axis < 4; ++axis) {
    matrix.at((axis * 4) + axis) = 1.0;
  }
  matrix.at(12) = offset_x;
  matrix.at(13) = offset_y;
  matrix.at(14) = offset_z;
  return matrix;
}

// A perspective camera whose image spans 1000 pixels per radian, with its
// near plane at 0.1 m.
constexpr ImageScale kPerspective{
    .projection = device::Projection::kPerspective,
    .pixels_per_unit = 1000.0,
    .near = 0.1,
};

TEST(PixelLengthTest, IsTheLengthOverTheDistanceForAPerspectiveCamera) {
  // 0.01 m at 10 m spans a milliradian: a pixel.
  EXPECT_DOUBLE_EQ(PixelLength(kPerspective, {0.0, 0.0, 0.0}, {0.0, 0.0, -10.0},
                               {0.01, 0.0, -10.0}),
                   1.0);
  EXPECT_DOUBLE_EQ(PixelLength(kPerspective, {0.0, 0.0, 0.0}, {0.0, 0.0, -5.0},
                               {0.01, 0.0, -5.0}),
                   2.0);
}

TEST(PixelLengthTest, MeasuresFromTheEndClosestToTheEye) {
  // A segment along the view, from 1 m to 2 m away, is 1 m at 1 m.
  EXPECT_DOUBLE_EQ(PixelLength(kPerspective, {0.0, 0.0, 0.0}, {0.0, 0.0, -2.0},
                               {0.0, 0.0, -1.0}),
                   1000.0);
}

TEST(PixelLengthTest, MeasuresNoCloserThanTheNearPlane) {
  EXPECT_DOUBLE_EQ(PixelLength(kPerspective, {0.0, 0.0, 0.0}, {0.0, 0.0, -0.01},
                               {0.001, 0.0, -0.01}),
                   10.0);
}

TEST(PixelLengthTest, IsTheLengthTimesThePixelsPerMeterWhenOrthographic) {
  const ImageScale orthographic{
      .projection = device::Projection::kOrthographic,
      .pixels_per_unit = 200.0,
      .near = 0.1,
  };
  EXPECT_DOUBLE_EQ(PixelLength(orthographic, {0.0, 0.0, 0.0},
                               {0.0, 0.0, -100.0}, {0.5, 0.0, -100.0}),
                   100.0);
}

TEST(EdgeRateTest, CutsAnEdgeIntoSegmentsNoLongerThanTheRate) {
  EXPECT_EQ(EdgeRate(10.0, 1.0), 10U);
  EXPECT_EQ(EdgeRate(10.5, 1.0), 11U);
  EXPECT_EQ(EdgeRate(10.0, 4.0), 3U);
  EXPECT_EQ(EdgeRate(0.5, 0.25), 2U);
}

TEST(EdgeRateTest, CutsAnEdgeIntoOneSegmentAtLeast) {
  EXPECT_EQ(EdgeRate(0.0, 1.0), 1U);
  EXPECT_EQ(EdgeRate(0.2, 1.0), 1U);
}

TEST(EdgeRateTest, CutsAnEdgeIntoTheMostSegmentsAtMost) {
  EXPECT_EQ(EdgeRate(1e9, 1.0), kMaxEdgeRate);
  EXPECT_EQ(EdgeRate(std::numeric_limits<double>::infinity(), 1.0),
            kMaxEdgeRate);
  EXPECT_EQ(EdgeRate(std::numeric_limits<double>::quiet_NaN(), 1.0),
            kMaxEdgeRate);
}

// A unit cube's corners, in object space.
const std::vector<device::Float3> kCube{
    {.x = -0.5F, .y = -0.5F, .z = -0.5F},
    {.x = 0.5F, .y = 0.5F, .z = 0.5F},
};

TEST(ClosestViewpointTest, IsTheInstanceClosestToTheEye) {
  const std::array<device::Matrix, 1> far{Translation(0.0, 0.0, -10.0)};
  const std::array<device::Matrix, 1> near{Translation(3.0, 0.0, 0.0)};
  const std::array<std::span<const device::Matrix>, 2> instances{far, near};
  const std::array<Point3, 1> eyes{Point3{0.0, 0.0, 0.0}};
  const Viewpoint viewpoint = ClosestViewpoint(kCube, instances, eyes);
  EXPECT_EQ(viewpoint.object_to_world, near.at(0));
  EXPECT_EQ(viewpoint.eye, eyes.at(0));
}

TEST(ClosestViewpointTest, IsTheClosestPairOfSamplesOverTheShutter) {
  // The instance moves towards x, the camera towards -x: they are closest
  // at the instance's first sample and the camera's first.
  const std::array<device::Matrix, 2> moving{
      Translation(2.0, 0.0, 0.0),
      Translation(6.0, 0.0, 0.0),
  };
  const std::array<std::span<const device::Matrix>, 1> instances{moving};
  const std::array<Point3, 2> eyes{
      Point3{0.0, 0.0, 0.0},
      Point3{-4.0, 0.0, 0.0},
  };
  const Viewpoint viewpoint = ClosestViewpoint(kCube, instances, eyes);
  EXPECT_EQ(viewpoint.object_to_world, moving.at(0));
  EXPECT_EQ(viewpoint.eye, eyes.at(0));
}

TEST(ClosestViewpointTest, MeasuresToTheBoundsOfTheInstance) {
  // A long box whose centre is farther than a small one's, but whose end
  // is closer.
  const std::vector<device::Float3> long_box{
      {.x = -5.0F, .y = -0.5F, .z = -0.5F},
      {.x = 5.0F, .y = 0.5F, .z = 0.5F},
  };
  const std::array<device::Matrix, 1> far{Translation(6.0, 0.0, 0.0)};
  const std::array<device::Matrix, 1> near{Translation(0.0, 3.0, 0.0)};
  const std::array<std::span<const device::Matrix>, 2> instances{far, near};
  const std::array<Point3, 1> eyes{Point3{0.0, 0.0, 0.0}};
  EXPECT_EQ(ClosestViewpoint(long_box, instances, eyes).object_to_world,
            far.at(0));
}

TEST(ClosestViewpointTest, IsTheIdentityWithNoEye) {
  const std::array<device::Matrix, 1> placed{Translation(1.0, 2.0, 3.0)};
  const std::array<std::span<const device::Matrix>, 1> instances{placed};
  const Viewpoint viewpoint = ClosestViewpoint(kCube, instances, {});
  EXPECT_EQ(viewpoint.object_to_world, Viewpoint{}.object_to_world);
  EXPECT_EQ(viewpoint.eye, (Point3{0.0, 0.0, 0.0}));
}

TEST(ToWorldTest, TransformsARowVector) {
  // A quarter turn about z, then a move by (1, 2, 3).
  device::Matrix turn = Translation(1.0, 2.0, 3.0);
  turn.at(0) = 0.0;
  turn.at(1) = 1.0;
  turn.at(4) = -1.0;
  turn.at(5) = 0.0;
  const Point3 world = ToWorld(turn, {.x = 1.0F, .y = 0.0F, .z = 0.0F});
  EXPECT_DOUBLE_EQ(world.at(0), 1.0);
  EXPECT_DOUBLE_EQ(world.at(1), 3.0);
  EXPECT_DOUBLE_EQ(world.at(2), 3.0);
}

}  // namespace
}  // namespace umbra::ext::usd::internal::subdivision
