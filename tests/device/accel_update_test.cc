// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/accel_update.h"

#include <gtest/gtest.h>

#include <array>
#include <span>

#include "umbra/device/change_set.h"
#include "umbra/device/render_data.h"

namespace umbra::device {
namespace {

constexpr AccelPoints kPoints{.motion_samples = 2, .count = 3};

// A refittable acceleration structure built from kPoints, refitted
// `refits` times since.
HeldAccel RefittableAfter(int refits) {
  return {
      .kind = AccelKind::kRefittable,
      .points = kPoints,
      .refits = refits,
  };
}

TEST(AccelPointsOfTest, CountsTheMotionSamplesAndThePointsOfEach) {
  const std::array<Float3, 3> first{};
  const std::array<Float3, 3> second{};
  const std::array<std::span<const Float3>, 2> samples = {first, second};
  Prototype prototype;
  prototype.points = samples;
  EXPECT_EQ(AccelPointsOf(prototype), kPoints);
}

TEST(AccelPointsOfTest, CountsNothingWithoutPoints) {
  EXPECT_EQ(AccelPointsOf(Prototype{}), AccelPoints{});
}

TEST(PlanAccelUpdateTest, KeepsItWhenNeitherTopologyNorPointsChange) {
  EXPECT_EQ(
      PlanAccelUpdate(RefittableAfter(0), ChangeFlags::kPrimvars, kPoints),
      AccelUpdate::kKeep);
}

TEST(PlanAccelUpdateTest, RebuildsItWhenTheTopologyChanges) {
  EXPECT_EQ(
      PlanAccelUpdate(RefittableAfter(0),
                      ChangeFlags::kTopology | ChangeFlags::kPoints, kPoints),
      AccelUpdate::kRebuildTopology);
}

TEST(PlanAccelUpdateTest, RebuildsAStaticOneToBeRefitted) {
  const HeldAccel held{
      .kind = AccelKind::kStatic,
      .points = kPoints,
      .refits = 0,
  };
  EXPECT_EQ(PlanAccelUpdate(held, ChangeFlags::kPoints, kPoints),
            AccelUpdate::kRebuildToRefit);
}

TEST(PlanAccelUpdateTest, RefitsARefittableOne) {
  EXPECT_EQ(PlanAccelUpdate(RefittableAfter(0), ChangeFlags::kPoints, kPoints),
            AccelUpdate::kRefit);
  EXPECT_EQ(PlanAccelUpdate(RefittableAfter(kMaxRefits - 1),
                            ChangeFlags::kPoints, kPoints),
            AccelUpdate::kRefit);
}

TEST(PlanAccelUpdateTest, RebuildsItWhenTheMotionSamplesChange) {
  EXPECT_EQ(PlanAccelUpdate(RefittableAfter(0), ChangeFlags::kPoints,
                            {.motion_samples = 1, .count = 3}),
            AccelUpdate::kRebuildPoints);
}

TEST(PlanAccelUpdateTest, RebuildsItWhenThePointCountChanges) {
  EXPECT_EQ(PlanAccelUpdate(RefittableAfter(0), ChangeFlags::kPoints,
                            {.motion_samples = 2, .count = 4}),
            AccelUpdate::kRebuildPoints);
}

TEST(PlanAccelUpdateTest, RebuildsItAfterMaxRefits) {
  EXPECT_EQ(PlanAccelUpdate(RefittableAfter(kMaxRefits), ChangeFlags::kPoints,
                            kPoints),
            AccelUpdate::kRebuildAfterRefits);
}

}  // namespace
}  // namespace umbra::device
