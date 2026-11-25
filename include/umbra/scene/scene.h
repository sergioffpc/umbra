// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The Scene: what umbra renders, in umbra's own types, with no OpenUSD type
// (docs/adr/0006-scenes-enter-through-hydra.md). The OpenUSD bridge builds it
// (umbra::ext::usd::LoadScene) and it is immutable once built. Lengths are in
// meters.

#ifndef UMBRA_SCENE_SCENE_H_
#define UMBRA_SCENE_SCENE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::scene {

// How a camera projects the scene onto its aperture.
enum class Projection : std::uint8_t {
  kPerspective,
  kOrthographic,
};

// A camera, as the stage's Camera prim describes it, in meters.
struct Camera {
  // The path of its prim ("/World/cam").
  std::string path;
  Projection projection = Projection::kPerspective;
  // The camera-to-world transform, row-major, for row vectors: the
  // translation is the last row. The camera looks down its -Z axis, +Y up.
  std::array<double, 16> camera_to_world{};
  double focal_length = 0.0;
  // The aperture, horizontal then vertical, and its offset from the axis.
  std::array<double, 2> aperture{};
  std::array<double, 2> aperture_offset{};
  // The near and far clipping distances.
  std::array<double, 2> clipping_range{};
  // The f-number of the lens; 0 for a pinhole, with no depth of field.
  double f_stop = 0.0;
  // The distance in focus, with depth of field.
  double focus_distance = 0.0;
  // The shutter: when it opens and closes, in frames relative to the frame.
  std::array<double, 2> shutter{};
};

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
  // The camera it sees through: an index into Scene::cameras().
  std::size_t camera = 0;
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
  // Its camera: an index into Scene::cameras().
  std::size_t camera = 0;
  // At least one product.
  std::vector<RenderProduct> products;
};

// The scene of one frame.
class Scene {
 public:
  // The cameras are those the settings and their products see through.
  Scene(RenderSettings settings, std::vector<Camera> cameras);

  [[nodiscard]] const RenderSettings& settings() const UMBRA_LIFETIMEBOUND {
    return settings_;
  }
  [[nodiscard]] std::span<const Camera> cameras() const UMBRA_LIFETIMEBOUND {
    return cameras_;
  }

 private:
  RenderSettings settings_;
  std::vector<Camera> cameras_;
};

}  // namespace umbra::scene

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_SCENE_SCENE_H_
