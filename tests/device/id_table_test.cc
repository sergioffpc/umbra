// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/id_table.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "umbra/device/camera_rays.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/device/image.h"
#include "umbra/device/render_data.h"

namespace umbra::device {
namespace {

using Table = IdTable<std::string, LightId>;
using Entries = std::vector<std::pair<std::uint32_t, std::string>>;

// The light of id `id`.
Light LightOf(std::uint32_t key) {
  Light light;
  light.id = LightId{key};
  return light;
}

// A table of `count` ids that holds `held` under each id of `ids`.
Table TableOf(std::uint32_t count, const Entries& held) {
  Table table;
  table.Reserve(count);
  for (const auto& [key, value] : held) {
    EXPECT_TRUE(table.Add(LightId{key}, value).has_value());
  }
  return table;
}

// The id and the value of everything `table` holds, by id.
Entries Held(const Table& table) {
  Entries held;
  table.ForEach([&](LightId key, const std::string& value) {
    held.emplace_back(std::to_underlying(key), value);
  });
  return held;
}

// The changes of lights that add `added`, update `updated` and remove
// `removed`.
Changes<Light, LightId> LightChanges(std::span<const Light> added,
                                     std::span<const Light> updated,
                                     std::span<const LightId> removed) {
  return {
      .added = added,
      .updated = updated,
      .removed = removed,
  };
}

// What `table` holds under `key`, or "(none)".
std::string HeldAt(const Table& table, LightId key) {
  const std::string* const held = table.Find(key);
  return held == nullptr ? std::string("(none)") : *held;
}

void ExpectFault(const IdStatus& status, IdFault fault, std::uint32_t key) {
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().fault, fault);
  EXPECT_EQ(status.error().id, key);
}

TEST(IdTableTest, HoldsWhatIsAddedUnderItsId) {
  Table table = TableOf(3, {{1, "one"}});

  EXPECT_TRUE(table.Contains(LightId{1}));
  EXPECT_FALSE(table.Contains(LightId{0}));
  EXPECT_EQ(HeldAt(table, LightId{1}), "one");
  EXPECT_EQ(table.Find(LightId{2}), nullptr);
  EXPECT_EQ(table.size(), 3U);
}

TEST(IdTableTest, FindsNothingOutOfRange) {
  const Table table = TableOf(2, {});

  EXPECT_EQ(table.Find(LightId{7}), nullptr);
  EXPECT_FALSE(table.Contains(LightId{7}));
}

TEST(IdTableTest, RefusesToAddAnIdHeld) {
  Table table = TableOf(2, {{0, "zero"}});

  ExpectFault(table.Add(LightId{0}, "again"), IdFault::kAddedHeld, 0);
  EXPECT_EQ(HeldAt(table, LightId{0}), "zero");
}

TEST(IdTableTest, RefusesToAddAnIdOutOfRange) {
  Table table = TableOf(2, {});

  ExpectFault(table.Add(LightId{2}, "two"), IdFault::kOutOfRange, 2);
}

TEST(IdTableTest, RemovesAnIdHeld) {
  Table table = TableOf(2, {{1, "one"}});

  EXPECT_TRUE(table.Remove(LightId{1}).has_value());
  EXPECT_FALSE(table.Contains(LightId{1}));
  ExpectFault(table.Remove(LightId{1}), IdFault::kRemovedNotHeld, 1);
  ExpectFault(table.Remove(LightId{5}), IdFault::kOutOfRange, 5);
}

TEST(IdTableTest, KeepsAValueWhereItIsWhenItGrows) {
  Table table = TableOf(1, {{0, "zero"}});
  const std::string* const before = table.Find(LightId{0});

  table.Reserve(1000);

  EXPECT_EQ(table.Find(LightId{0}), before);
  EXPECT_EQ(table.size(), 1000U);
}

TEST(IdTableTest, NeverShrinksWhenItReserves) {
  Table table = TableOf(4, {{3, "three"}});

  table.Reserve(2);

  EXPECT_EQ(table.size(), 4U);
  EXPECT_TRUE(table.Contains(LightId{3}));
}

TEST(IdTableTest, VisitsWhatItHoldsByIdInOrder) {
  const Table table = TableOf(4, {{3, "three"}, {1, "one"}});

  EXPECT_EQ(Held(table), (Entries{{1, "one"}, {3, "three"}}));
}

TEST(IdTableTest, CopiesWhatItHolds) {
  const Table table = TableOf(2, {{1, "one"}});

  Table copy(table);
  std::string* const held = copy.Find(LightId{1});
  if (held != nullptr) {
    *held = "changed";
  }

  EXPECT_EQ(HeldAt(table, LightId{1}), "one");
  EXPECT_EQ(Held(copy), (Entries{{1, "changed"}}));
}

TEST(IdTableTest, ShrinksToAnIdCountAboveWhatItHolds) {
  Table table = TableOf(4, {{1, "one"}});

  EXPECT_TRUE(table.Shrink(2).has_value());
  EXPECT_EQ(table.size(), 2U);
  EXPECT_TRUE(table.Contains(LightId{1}));
}

TEST(IdTableTest, RefusesToShrinkBelowAnIdHeld) {
  Table table = TableOf(4, {{3, "three"}});

  ExpectFault(table.Shrink(2), IdFault::kHeldOutOfRange, 3);
  EXPECT_EQ(table.size(), 4U);
}

TEST(IdTableTest, DropsEverythingWhenCleared) {
  Table table = TableOf(2, {{0, "zero"}});

  table.Clear();

  EXPECT_FALSE(table.Contains(LightId{0}));
  EXPECT_EQ(table.size(), 0U);
}

TEST(ApplyIdsTest, RemovesThenChecksTheUpdatesThenAdds) {
  Table table = TableOf(3, {{0, "zero"}, {1, "one"}});
  const std::array added = {LightOf(0), LightOf(2)};
  const std::array updated = {LightOf(1)};
  const std::array removed = {LightId{0}};
  EXPECT_TRUE(
      ApplyIds(LightChanges(added, updated, removed), 3, table).has_value());

  // The ids added hold a value made anew; the id updated keeps its own.
  EXPECT_EQ(Held(table), (Entries{{0, ""}, {1, "one"}, {2, ""}}));
}

TEST(ApplyIdsTest, GrowsTheTableToTheIdCount) {
  Table table;
  const std::array added = {LightOf(4)};

  EXPECT_TRUE(ApplyIds(LightChanges(added, {}, {}), 5, table).has_value());

  EXPECT_EQ(table.size(), 5U);
  EXPECT_TRUE(table.Contains(LightId{4}));
}

TEST(ApplyIdsTest, RefusesAnIdNotBelowTheCount) {
  Table table = TableOf(8, {});
  const std::array added = {LightOf(5)};

  ExpectFault(ApplyIds(LightChanges(added, {}, {}), 4, table),
              IdFault::kOutOfRange, 5);
}

TEST(ApplyIdsTest, RefusesToRemoveAnIdNotHeld) {
  Table table = TableOf(2, {});
  const std::array removed = {LightId{1}};

  ExpectFault(ApplyIds(LightChanges({}, {}, removed), 2, table),
              IdFault::kRemovedNotHeld, 1);
}

TEST(ApplyIdsTest, RefusesToUpdateAnIdRemoved) {
  Table table = TableOf(2, {{1, "one"}});
  const std::array updated = {LightOf(1)};
  const std::array removed = {LightId{1}};

  ExpectFault(ApplyIds(LightChanges({}, updated, removed), 2, table),
              IdFault::kUpdatedNotHeld, 1);
}

TEST(ApplyIdsTest, RefusesToUpdateAnIdTwice) {
  Table table = TableOf(2, {{1, "one"}});
  const std::array updated = {LightOf(1), LightOf(1)};

  ExpectFault(ApplyIds(LightChanges({}, updated, {}), 2, table),
              IdFault::kUpdatedTwice, 1);
}

TEST(ApplyIdsTest, RefusesToAddAnIdHeld) {
  Table table = TableOf(2, {{1, "one"}});
  const std::array added = {LightOf(1)};

  ExpectFault(ApplyIds(LightChanges(added, {}, {}), 2, table),
              IdFault::kAddedHeld, 1);
}

TEST(ApplyIdsTest, ReadsTheIdsOfUpdatedPrototypes) {
  IdTable<int, PrototypeId> table;
  table.Reserve(2);
  ASSERT_TRUE(table.Add(PrototypeId{1}, 7).has_value());
  Prototype prototype;
  prototype.id = PrototypeId{0};
  const std::array updated = {
      UpdatedPrototype{
          .prototype = prototype,
          .changes = ChangeFlags::kPoints,
      },
  };
  const PrototypeChanges changes{
      .added = {},
      .updated = updated,
      .removed = {},
  };

  ExpectFault(ApplyIds(changes, 2, table), IdFault::kUpdatedNotHeld, 0);
}

// A camera held as itself.
Camera Itself(const Camera& held) { return held; }

// The frame parameters of camera `camera`, for a 4x4 image.
FrameParameters FrameOf(CameraId camera) {
  return {
      .shutter = {},
      .camera = camera,
      .image =
          {
              .resolution = {4, 4},
              .window = {.x_min = 0, .y_min = 0, .x_end = 4, .y_end = 4},
              .pixel_aspect_ratio = 1.0F,
              .aspect_ratio_conform_policy =
                  AspectRatioConformPolicy::kExpandAperture,
          },
      .motion_blur = MotionBlur::kEnabled,
      .depth_of_field = DepthOfField::kEnabled,
      .samples_per_pixel = 1,
      .vars = {},
  };
}

TEST(FrameCameraModelTest, MakesTheModelOfTheFramesCamera) {
  IdTable<Camera, CameraId> cameras;
  cameras.Reserve(2);
  Camera camera;
  camera.id = CameraId{1};
  camera.projection = Projection::kOrthographic;
  camera.aperture = {2.0, 2.0};
  ASSERT_TRUE(cameras.Add(camera.id, camera).has_value());

  const std::optional<CameraModel> model =
      FrameCameraModel(cameras, FrameOf(CameraId{1}), Itself);

  EXPECT_EQ(model.value_or(CameraModel{}).projection,
            Projection::kOrthographic);
}

TEST(FrameCameraModelTest, MakesNoneWithoutTheFramesCamera) {
  IdTable<Camera, CameraId> cameras;
  cameras.Reserve(2);

  EXPECT_FALSE(
      FrameCameraModel(cameras, FrameOf(CameraId{1}), Itself).has_value());
}

}  // namespace
}  // namespace umbra::device
