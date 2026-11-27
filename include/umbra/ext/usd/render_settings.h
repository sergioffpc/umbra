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
#include "umbra/device/image.h"
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
  // The radiance of the light paths a light path expression matches.
  kLpe,
  // A primvar of the first hit, interpolated.
  kPrimvar,
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
  // The light path expression of a kLpe var ("C<RD>L"), or the name of the
  // primvar of a kPrimvar var ("st"): its sourceName, or its name when it has
  // none; empty for the others.
  std::string source_name;
  ChannelType type = ChannelType::kHalf;
  // The channels of its layer: 4 for color (RGBA), 1 for depth, 3 for
  // normal, albedo and lpe vars, and as many as its dataType has components
  // for a primvar var.
  std::uint32_t channels = 1;
};

using device::AspectRatioConformPolicy;
using device::DepthOfField;
using device::MotionBlur;

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
  // Those pixels: those whose centers lie in data_window_ndc. Never empty.
  device::PixelWindow window;
  MotionBlur motion_blur = MotionBlur::kEnabled;
  DepthOfField depth_of_field = DepthOfField::kEnabled;
  // The samples each pixel takes, umbra:samplesPerPixel: at least 1.
  std::uint32_t samples_per_pixel = 64;
  // Its camera's transforms at the frame, in meters, row-major for row
  // vectors, as OpenEXR's worldToCamera and worldToNDC give them: to the
  // camera's own space, and to the NDC of its screen window, (0, 0) at the
  // top left and (1, 1) at the bottom right once divided by w.
  device::Matrix world_to_camera{};
  device::Matrix world_to_ndc{};
  // The pixel aspect ratio of its file: pixel_aspect_ratio, or that which
  // the conform policy kAdjustPixelAspectRatio gives.
  float file_pixel_aspect_ratio = 1.0F;
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
