// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "render_vars.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "umbra/delegate/render_settings.h"
#include "umbra/device/image.h"

namespace umbra::delegate::internal {
namespace {

TEST(RenderVarOfTest, NamesTheAovOfEachSource) {
  EXPECT_EQ(RenderVarOf(RenderVarSource::kColor).name, "color");
  EXPECT_EQ(RenderVarOf(RenderVarSource::kDenoisedColor).name, "denoisedColor");
  EXPECT_EQ(RenderVarOf(RenderVarSource::kDepth).name, "depth");
  EXPECT_EQ(RenderVarOf(RenderVarSource::kNormal).name, "normal");
  EXPECT_EQ(RenderVarOf(RenderVarSource::kAlbedo).name, "albedo");
  EXPECT_EQ(RenderVarOf(RenderVarSource::kLpe).name, "lpe");
  EXPECT_EQ(RenderVarOf(RenderVarSource::kPrimvar).name, "primvar");
}

TEST(RenderVarOfTest, GivesARawVarToEverySourceButLpeAndPrimvar) {
  EXPECT_EQ(RenderVarOf(RenderVarSource::kColor).kind, RenderVarKind::kRaw);
  EXPECT_EQ(RenderVarOf(RenderVarSource::kDenoisedColor).kind,
            RenderVarKind::kRaw);
  EXPECT_EQ(RenderVarOf(RenderVarSource::kDepth).kind, RenderVarKind::kRaw);
  EXPECT_EQ(RenderVarOf(RenderVarSource::kNormal).kind, RenderVarKind::kRaw);
  EXPECT_EQ(RenderVarOf(RenderVarSource::kAlbedo).kind, RenderVarKind::kRaw);
  EXPECT_EQ(RenderVarOf(RenderVarSource::kLpe).kind, RenderVarKind::kNamed);
  EXPECT_EQ(RenderVarOf(RenderVarSource::kPrimvar).kind, RenderVarKind::kNamed);
}

TEST(RenderVarOfTest, HasTheDevicesRenderOnlyColorDepthNormalAndAlbedo) {
  EXPECT_EQ(RenderVarOf(RenderVarSource::kColor).rendered,
            device::VarSource::kColor);
  EXPECT_EQ(RenderVarOf(RenderVarSource::kDepth).rendered,
            device::VarSource::kDepth);
  EXPECT_EQ(RenderVarOf(RenderVarSource::kNormal).rendered,
            device::VarSource::kNormal);
  EXPECT_EQ(RenderVarOf(RenderVarSource::kAlbedo).rendered,
            device::VarSource::kAlbedo);
  EXPECT_FALSE(
      RenderVarOf(RenderVarSource::kDenoisedColor).rendered.has_value());
  EXPECT_FALSE(RenderVarOf(RenderVarSource::kLpe).rendered.has_value());
  EXPECT_FALSE(RenderVarOf(RenderVarSource::kPrimvar).rendered.has_value());
}

TEST(RenderVarOfTest, GivesTheChannelsOfEachSource) {
  EXPECT_EQ(RenderVarOf(RenderVarSource::kColor).channels, 4U);
  EXPECT_EQ(RenderVarOf(RenderVarSource::kDenoisedColor).channels, 4U);
  EXPECT_EQ(RenderVarOf(RenderVarSource::kDepth).channels, 1U);
  EXPECT_EQ(RenderVarOf(RenderVarSource::kNormal).channels, 3U);
  EXPECT_EQ(RenderVarOf(RenderVarSource::kAlbedo).channels, 3U);
  EXPECT_EQ(RenderVarOf(RenderVarSource::kLpe).channels, 3U);
  // A primvar var has as many as its dataType has components.
  EXPECT_EQ(RenderVarOf(RenderVarSource::kPrimvar).channels, 0U);
}

TEST(RenderVarsTest, DenoiseFromTheColorTheNormalAndTheAlbedo) {
  std::vector<RenderVarSource> inputs;
  for (const RenderVarTraits& var : kRenderVars) {
    if (var.denoise == DenoiseRole::kInput) {
      inputs.push_back(var.source);
    }
  }

  const std::vector<RenderVarSource> expected = {
      RenderVarSource::kColor,
      RenderVarSource::kNormal,
      RenderVarSource::kAlbedo,
  };
  EXPECT_EQ(inputs, expected);
}

TEST(ImageOfTest, FramesTheImageOfAProduct) {
  constexpr device::PixelWindow kWindow = {
      .x_min = 10,
      .y_min = 20,
      .x_end = 630,
      .y_end = 460,
  };
  RenderProduct product;
  product.resolution = {640, 480};
  product.window = kWindow;
  product.pixel_aspect_ratio = 2.0F;
  product.aspect_ratio_conform_policy = AspectRatioConformPolicy::kCropAperture;

  const device::Image image = ImageOf(product);

  EXPECT_EQ(image.resolution, (std::array<std::int32_t, 2>{640, 480}));
  EXPECT_EQ(image.window, kWindow);
  EXPECT_FLOAT_EQ(image.pixel_aspect_ratio, 2.0F);
  EXPECT_EQ(image.aspect_ratio_conform_policy,
            AspectRatioConformPolicy::kCropAperture);
}

}  // namespace
}  // namespace umbra::delegate::internal
