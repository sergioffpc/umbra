// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "buffer_layout.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "umbra/delegate/render_settings.h"

namespace umbra::delegate::internal {
namespace {

// A var of `source` with `channels` channels.
RenderVar Var(std::string name, RenderVarSource source,
              std::uint32_t channels) {
  return {
      .name = std::move(name),
      .source = source,
      .source_name = {},
      .type = ChannelType::kFloat,
      .channels = channels,
  };
}

// A product of `vars` over a 640x480 data window.
RenderProduct Product(std::vector<RenderVar> vars) {
  RenderProduct product;
  product.file = "render/beauty.0001.exr";
  product.resolution = {640, 480};
  product.window = {.x_min = 0, .y_min = 0, .x_end = 640, .y_end = 480};
  product.vars = std::move(vars);
  return product;
}

TEST(LayoutOfTest, GivesABufferPerVarInItsOrderOverTheDataWindow) {
  RenderProduct product = Product({
      Var("color", RenderVarSource::kColor, 4),
      Var("depth", RenderVarSource::kDepth, 1),
  });
  product.window = {.x_min = 10, .y_min = 20, .x_end = 110, .y_end = 70};

  const BufferLayout layout = LayoutOf(product);

  EXPECT_EQ(layout.width, 100);
  EXPECT_EQ(layout.height, 50);
  const std::vector<BufferSlot> expected = {
      {.source = RenderVarSource::kColor, .channels = 4},
      {.source = RenderVarSource::kDepth, .channels = 1},
  };
  EXPECT_EQ(layout.slots, expected);
}

TEST(LayoutOfTest, AddsTheDenoiseInputsTheProductHasNoVarOf) {
  const RenderProduct product = Product({
      Var("denoised", RenderVarSource::kDenoisedColor, 4),
      Var("color", RenderVarSource::kColor, 4),
  });

  const std::vector<BufferSlot> expected = {
      {.source = RenderVarSource::kDenoisedColor, .channels = 4},
      {.source = RenderVarSource::kColor, .channels = 4},
      {.source = RenderVarSource::kNormal, .channels = 3},
      {.source = RenderVarSource::kAlbedo, .channels = 3},
  };
  EXPECT_EQ(LayoutOf(product).slots, expected);
}

TEST(LayoutOfTest, AddsAnRgbaColorToADenoiseWithNoColorVar) {
  const RenderProduct product = Product({
      Var("denoised", RenderVarSource::kDenoisedColor, 4),
      Var("normal", RenderVarSource::kNormal, 3),
  });

  const std::vector<BufferSlot> expected = {
      {.source = RenderVarSource::kDenoisedColor, .channels = 4},
      {.source = RenderVarSource::kNormal, .channels = 3},
      {.source = RenderVarSource::kColor, .channels = 4},
      {.source = RenderVarSource::kAlbedo, .channels = 3},
  };
  EXPECT_EQ(LayoutOf(product).slots, expected);
}

TEST(LayoutOfTest, IsTheSameOnTheNextFrameOfTheSameImages) {
  const RenderProduct first = Product({
      Var("color", RenderVarSource::kColor, 4),
  });
  RenderProduct next = first;
  next.file = "render/beauty.0002.exr";
  next.samples_per_pixel = 16;
  next.vars.front().name = "beauty";

  EXPECT_EQ(LayoutOf(first), LayoutOf(next));
}

TEST(LayoutOfTest, ChangesWithTheDataWindowsSize) {
  const RenderProduct first = Product({
      Var("color", RenderVarSource::kColor, 4),
  });
  RenderProduct next = first;
  next.window.x_end = 320;

  EXPECT_NE(LayoutOf(first), LayoutOf(next));
}

TEST(LayoutOfTest, ChangesWithTheVars) {
  const RenderProduct first = Product({
      Var("primvar", RenderVarSource::kPrimvar, 2),
  });
  RenderProduct channels = first;
  channels.vars.front().channels = 3;
  RenderProduct added = first;
  added.vars.push_back(Var("depth", RenderVarSource::kDepth, 1));

  EXPECT_NE(LayoutOf(first), LayoutOf(channels));
  EXPECT_NE(LayoutOf(first), LayoutOf(added));
}

}  // namespace
}  // namespace umbra::delegate::internal
