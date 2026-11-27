// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/light_shapes.h"

#include <gtest/gtest.h>

#include "umbra/device/geometry.h"

namespace umbra::device {
namespace {

constexpr LightShape kSphere{.kind = LightShapeKind::kSphere, .radius = 1.0F};
constexpr LightShape kRect{
    .kind = LightShapeKind::kRect,
    .width = 2.0F,
    .height = 1.0F,
};
constexpr LightShape kDisk{.kind = LightShapeKind::kDisk, .radius = 1.0F};
constexpr LightShape kCylinder{
    .kind = LightShapeKind::kCylinder,
    .radius = 0.5F,
    .length = 2.0F,
};

// A ray from `origin` along `direction`, over every distance ahead.
ShapeRay RayFrom(Vector3 origin, Vector3 direction) {
  return {
      .origin = origin,
      .direction = direction,
      .t_min = 0.0F,
      .t_max = kNoHit,
  };
}

constexpr Vector3 kAlongX{.x = 1.0F};
constexpr Vector3 kAlongZ{.z = 1.0F};
constexpr Vector3 kAgainstZ{.z = -1.0F};

void ExpectNormal(const ShapeHit& hit, Vector3 normal) {
  EXPECT_FLOAT_EQ(hit.normal.x, normal.x);
  EXPECT_FLOAT_EQ(hit.normal.y, normal.y);
  EXPECT_FLOAT_EQ(hit.normal.z, normal.z);
}

TEST(IntersectSphereTest, MeetsItsNearSide) {
  const ShapeHit hit = Intersect(kSphere, RayFrom({.z = -5.0F}, kAlongZ));

  ASSERT_TRUE(IsHit(hit));
  EXPECT_FLOAT_EQ(hit.distance, 4.0F);
  ExpectNormal(hit, {.z = -1.0F});
}

TEST(IntersectSphereTest, MeetsItsFarSideFromInside) {
  const ShapeHit hit = Intersect(kSphere, RayFrom({}, kAlongZ));

  ASSERT_TRUE(IsHit(hit));
  EXPECT_FLOAT_EQ(hit.distance, 1.0F);
  ExpectNormal(hit, {.z = 1.0F});
}

TEST(IntersectSphereTest, TakesTheLengthOfTheDirection) {
  const ShapeHit hit = Intersect(kSphere, RayFrom({.z = -5.0F}, {.z = 2.0F}));

  EXPECT_FLOAT_EQ(hit.distance, 2.0F);
}

TEST(IntersectSphereTest, MeetsAGrazingRayAtItsTangent) {
  const ShapeHit hit =
      Intersect(kSphere, RayFrom({.x = 1.0F, .z = -5.0F}, kAlongZ));

  ASSERT_TRUE(IsHit(hit));
  EXPECT_FLOAT_EQ(hit.distance, 5.0F);
  ExpectNormal(hit, {.x = 1.0F});
}

TEST(IntersectSphereTest, MissesARayJustOutside) {
  EXPECT_FALSE(
      IsHit(Intersect(kSphere, RayFrom({.x = 1.001F, .z = -5.0F}, kAlongZ))));
}

TEST(IntersectSphereTest, KeepsItsPrecisionFarAway) {
  constexpr LightShape kSmall{
      .kind = LightShapeKind::kSphere,
      .radius = 0.01F,
  };

  const ShapeHit hit = Intersect(kSmall, RayFrom({.z = -1.0e4F}, kAlongZ));

  ASSERT_TRUE(IsHit(hit));
  EXPECT_NEAR(hit.distance, 1.0e4F - 0.01F, 1.0e-3F);
}

TEST(IntersectSphereTest, MeetsNothingBeyondTheRaysRange) {
  ShapeRay ray = RayFrom({.z = -5.0F}, kAlongZ);
  ray.t_max = 3.9F;

  EXPECT_FALSE(IsHit(Intersect(kSphere, ray)));
}

TEST(IntersectSphereTest, MeetsTheFarSideBeyondTheRaysStart) {
  ShapeRay ray = RayFrom({.z = -5.0F}, kAlongZ);
  ray.t_min = 4.5F;

  EXPECT_FLOAT_EQ(Intersect(kSphere, ray).distance, 6.0F);
}

TEST(IntersectRectTest, FacesItsEmittingSideFromEitherSide) {
  const ShapeHit above = Intersect(kRect, RayFrom({.z = 5.0F}, kAgainstZ));
  const ShapeHit below = Intersect(kRect, RayFrom({.z = -5.0F}, kAlongZ));

  EXPECT_FLOAT_EQ(above.distance, 5.0F);
  ExpectNormal(above, {.z = -1.0F});
  EXPECT_FLOAT_EQ(below.distance, 5.0F);
  ExpectNormal(below, {.z = -1.0F});
}

TEST(IntersectRectTest, KeepsToItsWidthAndHeight) {
  EXPECT_TRUE(IsHit(Intersect(
      kRect, RayFrom({.x = 0.99F, .y = 0.49F, .z = 5.0F}, kAgainstZ))));
  EXPECT_FALSE(
      IsHit(Intersect(kRect, RayFrom({.x = 1.01F, .z = 5.0F}, kAgainstZ))));
  EXPECT_FALSE(
      IsHit(Intersect(kRect, RayFrom({.y = 0.51F, .z = 5.0F}, kAgainstZ))));
}

TEST(IntersectRectTest, MissesARayInItsPlane) {
  EXPECT_FALSE(IsHit(Intersect(kRect, RayFrom({.x = -5.0F}, kAlongX))));
}

TEST(IntersectRectTest, MeetsAGrazingRayAtItsCenter) {
  const ShapeHit hit = Intersect(
      kRect, RayFrom({.x = -5.0F, .z = 0.001F}, {.x = 1.0F, .z = -0.0002F}));

  ASSERT_TRUE(IsHit(hit));
  EXPECT_NEAR(hit.distance, 5.0F, 1.0e-3F);
}

TEST(IntersectDiskTest, IncludesItsRim) {
  const ShapeHit hit =
      Intersect(kDisk, RayFrom({.x = 1.0F, .z = 5.0F}, kAgainstZ));

  ASSERT_TRUE(IsHit(hit));
  EXPECT_FLOAT_EQ(hit.distance, 5.0F);
  ExpectNormal(hit, {.z = -1.0F});
}

TEST(IntersectDiskTest, MissesBeyondItsRadius) {
  EXPECT_FALSE(IsHit(Intersect(
      kDisk, RayFrom({.x = 0.75F, .y = 0.75F, .z = 5.0F}, kAgainstZ))));
}

TEST(IntersectDiskTest, MissesARayInItsPlane) {
  EXPECT_FALSE(IsHit(Intersect(kDisk, RayFrom({.x = -5.0F}, kAlongX))));
}

TEST(IntersectCylinderTest, MeetsItsSide) {
  const ShapeHit hit = Intersect(kCylinder, RayFrom({.z = -5.0F}, kAlongZ));

  ASSERT_TRUE(IsHit(hit));
  EXPECT_FLOAT_EQ(hit.distance, 4.5F);
  ExpectNormal(hit, {.z = -1.0F});
}

TEST(IntersectCylinderTest, MissesBeyondItsEnds) {
  EXPECT_FALSE(
      IsHit(Intersect(kCylinder, RayFrom({.x = 1.01F, .z = -5.0F}, kAlongZ))));
}

TEST(IntersectCylinderTest, MeetsItsInsideThroughAnOpenEnd) {
  // The ray enters by the end at x = -1, and meets the side from within.
  const ShapeHit hit =
      Intersect(kCylinder, RayFrom({.x = -3.0F}, {.x = 1.0F, .z = 0.2F}));

  ASSERT_TRUE(IsHit(hit));
  EXPECT_FLOAT_EQ(hit.distance, 2.5F);
  ExpectNormal(hit, {.z = 1.0F});
}

TEST(IntersectCylinderTest, MissesARayAlongItsAxis) {
  EXPECT_FALSE(IsHit(Intersect(kCylinder, RayFrom({.x = -5.0F}, kAlongX))));
}

TEST(IntersectCylinderTest, MeetsAGrazingRayAtItsTangent) {
  const ShapeHit hit =
      Intersect(kCylinder, RayFrom({.y = 0.5F, .z = -5.0F}, kAlongZ));

  ASSERT_TRUE(IsHit(hit));
  EXPECT_FLOAT_EQ(hit.distance, 5.0F);
  ExpectNormal(hit, {.y = 1.0F});
}

TEST(BoundsOfTest, BoundsEachShape) {
  const ShapeBounds cylinder = BoundsOf(kCylinder);

  EXPECT_FLOAT_EQ(cylinder.lower.x, -1.0F);
  EXPECT_FLOAT_EQ(cylinder.upper.y, 0.5F);
  EXPECT_FLOAT_EQ(BoundsOf(kRect).upper.x, 1.0F);
  EXPECT_FLOAT_EQ(BoundsOf(kRect).upper.z, 0.0F);
  EXPECT_FLOAT_EQ(BoundsOf(kSphere).lower.z, -1.0F);
}

}  // namespace
}  // namespace umbra::device
