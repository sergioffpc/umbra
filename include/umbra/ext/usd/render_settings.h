// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The render settings a render uses, as the bridge reads them from the
// stage. They stay on the host, where the render buffers are created and the
// render products written
// (docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_EXT_USD_RENDER_SETTINGS_H_
#define UMBRA_EXT_USD_RENDER_SETTINGS_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "umbra/core/safety.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd {

// The quantity a render var holds per pixel.
enum class RenderVarSource : std::uint8_t {
  // The radiance, RGBA.
  kColor,
  // The distance from the camera to the first hit, in meters.
  kDepth,
  // The world-space shading normal of the first hit.
  kNormal,
  // The diffuse reflectance of the first hit.
  kAlbedo,
};

// The type of the channels of a render var in its file.
enum class ChannelType : std::uint8_t {
  kHalf,
  kFloat,
};

// One quantity per pixel a render product holds: a layer of its file, named
// after the var ("color" gives color.R, color.G, ...).
struct RenderVar {
  std::string name;
  RenderVarSource source = RenderVarSource::kColor;
  ChannelType type = ChannelType::kHalf;
};

// How the aspect ratio of the aperture is conformed to that of the image
// when they differ, as UsdRender defines it.
enum class AspectRatioConformPolicy : std::uint8_t {
  kExpandAperture,
  kCropAperture,
  kAdjustApertureWidth,
  kAdjustApertureHeight,
  kAdjustPixelAspectRatio,
};

// Whether a render product integrates the shutter, or the frame's instant.
enum class MotionBlur : std::uint8_t {
  kEnabled,
  kDisabled,
};

// Whether a render product renders the lens's depth of field, or a pinhole.
enum class DepthOfField : std::uint8_t {
  kEnabled,
  kDisabled,
};

// An image file the render writes, an EXR whose layers are its vars.
struct RenderProduct {
  // The file, relative to the current directory unless absolute, with its
  // frame expanded ("render/beauty.0012.exr").
  std::string file;
  // The camera it sees through.
  device::CameraId camera{};
  // Its size in pixels, width then height.
  std::array<std::int32_t, 2> resolution{};
  float pixel_aspect_ratio = 1.0F;
  AspectRatioConformPolicy aspect_ratio_conform_policy =
      AspectRatioConformPolicy::kExpandAperture;
  // The pixels it holds, in NDC of the aperture: xmin, ymin, xmax, ymax, with
  // (0, 0) at the bottom left and (1, 1) at the top right.
  std::array<float, 4> data_window_ndc{};
  MotionBlur motion_blur = MotionBlur::kEnabled;
  DepthOfField depth_of_field = DepthOfField::kEnabled;
  // At least one var.
  std::vector<RenderVar> vars;
};

// The render settings the render uses.
struct RenderSettings {
  // The path of its prim ("/Render/main").
  std::string path;
  // Its camera.
  device::CameraId camera{};
  // At least one product.
  std::vector<RenderProduct> products;
};

}  // namespace umbra::ext::usd

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_RENDER_SETTINGS_H_
