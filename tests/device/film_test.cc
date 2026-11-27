// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/film.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "umbra/device/camera_rays.h"
#include "umbra/device/device.h"
#include "umbra/device/geometry.h"
#include "umbra/device/image.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/sampler.h"

namespace umbra::device {
namespace {

// A var of `source` and `channels`, with no render buffer.
FilmVar VarOf(VarSource source, std::uint32_t channels) {
  return {.source = source, .pixels = {}, .channels = channels};
}

// The frame of a product over `window`, with `vars`.
FrameParameters FrameOf(const PixelWindow& window,
                        std::span<const FilmVar> vars) {
  FrameParameters frame;
  frame.image.window = window;
  frame.vars = vars;
  return frame;
}

// The film's layout for `frame`, which fails the test when there is none.
FilmLayout Planned(const FrameParameters& frame) {
  const std::optional<FilmLayout> layout = PlanFilm(frame);
  if (!layout.has_value()) {
    ADD_FAILURE() << "no layout for " << frame.vars.size() << " vars";
    return {};
  }
  return *layout;
}

TEST(PlanFilmTest, LaysThePlanesOutOneAfterAnother) {
  const std::array vars = {
      VarOf(VarSource::kColor, 4),
      VarOf(VarSource::kDepth, 1),
      VarOf(VarSource::kNormal, 3),
  };
  // 4×3 pixels, overscanning the image on its left and top.
  const FilmLayout layout = Planned(
      FrameOf({.x_min = -1, .y_min = -1, .x_end = 3, .y_end = 2}, vars));
  ASSERT_EQ(layout.Planes().size(), 3U);
  EXPECT_EQ(layout.storage.at(0).offset, 0U);
  EXPECT_EQ(layout.storage.at(1).offset, 48U);
  EXPECT_EQ(layout.storage.at(2).offset, 60U);
  EXPECT_EQ(layout.floats, 96U);
  for (std::size_t index = 0; index < vars.size(); ++index) {
    EXPECT_EQ(layout.storage.at(index).source, vars.at(index).source);
    EXPECT_EQ(layout.storage.at(index).channels, vars.at(index).channels);
  }
}

TEST(PlanFilmTest, TakesNoFloatsForAnEmptyWindow) {
  const std::array vars = {VarOf(VarSource::kColor, 4)};
  const FilmLayout layout =
      Planned(FrameOf({.x_min = 2, .y_min = 2, .x_end = 1, .y_end = 4}, vars));
  EXPECT_EQ(layout.Planes().size(), 1U);
  EXPECT_EQ(layout.floats, 0U);
}

TEST(PlanFilmTest, HoldsAtMostItsCapacityOfPlanes) {
  const PixelWindow window{.x_end = 2, .y_end = 2};
  const std::vector<FilmVar> full(kMaxFilmPlanes);
  const FilmLayout layout = Planned(FrameOf(window, full));
  EXPECT_EQ(layout.Planes().size(), kMaxFilmPlanes);
  const std::vector<FilmVar> beyond(kMaxFilmPlanes + 1);
  EXPECT_FALSE(PlanFilm(FrameOf(window, beyond)).has_value());
}

TEST(PixelIndexTest, CountsRowByRowFromTheWindowsTopLeft) {
  const PixelWindow window{.x_min = -2, .y_min = -1, .x_end = 6, .y_end = 5};
  EXPECT_EQ(PixelIndex(window, -2, -1), 0U);
  EXPECT_EQ(PixelIndex(window, 5, -1), 7U);
  EXPECT_EQ(PixelIndex(window, -2, 0), 8U);
  EXPECT_EQ(PixelIndex(window, 5, 4), 47U);
}

TEST(TileRowOffsetTest, IsTheFirstFloatOfTheTilesRow) {
  const PixelWindow window{.x_min = -2, .y_min = -1, .x_end = 6, .y_end = 5};
  const PixelWindow tile{.x_min = 2, .y_min = 3, .x_end = 4, .y_end = 5};
  // The pixel (2, 4) is the fifth of the sixth row: 5·8 + 4 = 44.
  EXPECT_EQ(TileRowOffset(window, tile, 4, 3), 132U);
  EXPECT_EQ(TileRowOffset(window, tile, 3, 1), 36U);
}

TEST(WritePixelTest, WritesTheChannelsEachVarTakesAndZeroBeyond) {
  const std::array vars = {
      // Color into three channels: alpha is dropped.
      VarOf(VarSource::kColor, 3),
      // Depth into two: the second is 0.
      VarOf(VarSource::kDepth, 2),
      VarOf(VarSource::kNormal, 3),
      // Albedo, which the placeholder integrator leaves at 0.
      VarOf(VarSource::kAlbedo, 3),
  };
  const FilmLayout layout = Planned(FrameOf({.x_end = 2, .y_end = 1}, vars));
  std::vector<float> film(layout.floats, -1.0F);
  const PixelValue value{
      .color = {0.1F, 0.2F, 0.3F, 1.0F},
      .depth = 5.0F,
      .normal = {0.0F, 1.0F, 0.0F},
  };
  WritePixel(value, 1, layout, film);
  const std::vector<float> expected = {
      -1.0F, -1.0F, -1.0F, 0.1F, 0.2F, 0.3F,  // color
      -1.0F, -1.0F, 5.0F,  0.0F,              // depth
      -1.0F, -1.0F, -1.0F, 0.0F, 1.0F, 0.0F,  // normal
      -1.0F, -1.0F, -1.0F, 0.0F, 0.0F, 0.0F,  // albedo
  };
  EXPECT_EQ(film, expected);
}

// A camera that looks down -Z through a 4×4 image.
CameraModel TestCamera() {
  return {
      .resolution = {.x = 4.0F, .y = 4.0F},
      .screen_min = {.x = -1.0F, .y = -1.0F},
      .screen_size = {.x = 2.0F, .y = 2.0F},
      .near = 0.1F,
      .far = 100.0F,
  };
}

// A sampler cache, initialized.
std::vector<std::byte> SamplerCache() {
  std::vector<std::byte> cache(kSamplerCacheSize);
  InitializeSamplerCache(cache);
  return cache;
}

TEST(PlaceholderPixelTest, IsTheMeanOfItsSamples) {
  const CameraModel camera = TestCamera();
  const std::vector<std::byte> cache = SamplerCache();
  const PixelSampling sampling{
      .camera = &camera,
      .sampler_cache = cache.data(),
      .samples_per_pixel = 4,
  };
  int traced = 0;
  float towards = 0.0F;
  const auto trace = [&traced, &towards](const CameraRay& ray) {
    ++traced;
    towards = ray.direction.z;
    // Half the hits at 1, half at 3, with normals along X and then Y.
    const bool first = traced % 2 == 1;
    return FirstHit{
        .distance = first ? 1.0F : 3.0F,
        .normal = first ? Vector3{.x = 1.0F} : Vector3{.y = 1.0F},
    };
  };
  const PixelValue value = PlaceholderPixel(sampling, 1, 2, trace);
  EXPECT_EQ(traced, 4);
  EXPECT_LT(towards, 0.0F);
  EXPECT_FLOAT_EQ(value.depth, 2.0F);
  EXPECT_FLOAT_EQ(value.normal.at(0), 0.5F);
  EXPECT_FLOAT_EQ(value.normal.at(1), 0.5F);
  EXPECT_FLOAT_EQ(value.normal.at(2), 0.0F);
  EXPECT_EQ(value.color.at(3), 1.0F);
  // 0.5·direction + 0.5, with the direction towards -Z.
  for (std::size_t channel = 0; channel < 3; ++channel) {
    EXPECT_GE(value.color.at(channel), 0.0F);
    EXPECT_LE(value.color.at(channel), 1.0F);
  }
  EXPECT_LT(value.color.at(2), 0.5F);
}

TEST(PlaceholderPixelTest, IsAtNoHitWhenARayMeetsNothing) {
  const CameraModel camera = TestCamera();
  const std::vector<std::byte> cache = SamplerCache();
  const PixelSampling sampling{
      .camera = &camera,
      .sampler_cache = cache.data(),
      .samples_per_pixel = 3,
  };
  int traced = 0;
  const auto trace = [&traced](const CameraRay&) {
    ++traced;
    return traced == 2 ? FirstHit{} : FirstHit{.distance = 1.0F, .normal = {}};
  };
  EXPECT_EQ(PlaceholderPixel(sampling, 0, 0, trace).depth, kNoHit);
}

TEST(PlaceholderPixelTest, TakesOneSampleAtLeast) {
  const CameraModel camera = TestCamera();
  const std::vector<std::byte> cache = SamplerCache();
  const PixelSampling sampling{
      .camera = &camera,
      .sampler_cache = cache.data(),
      .samples_per_pixel = 0,
  };
  int traced = 0;
  const auto trace = [&traced](const CameraRay&) {
    ++traced;
    return FirstHit{.distance = 2.0F, .normal = {}};
  };
  EXPECT_FLOAT_EQ(PlaceholderPixel(sampling, 3, 3, trace).depth, 2.0F);
  EXPECT_EQ(traced, 1);
}

}  // namespace
}  // namespace umbra::device
