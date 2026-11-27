// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/camera_rays.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "umbra/device/device.h"
#include "umbra/device/geometry.h"
#include "umbra/device/image.h"
#include "umbra/device/render_data.h"

namespace umbra::device {
namespace {

// The row of `matrix` at `row`, as a vector.
Vector3 RowOf(const Matrix& matrix, std::size_t row) {
  return {
      .x = static_cast<float>(matrix.at(row * 4)),
      .y = static_cast<float>(matrix.at((row * 4) + 1)),
      .z = static_cast<float>(matrix.at((row * 4) + 2)),
  };
}

Affine AffineOf(const Matrix& matrix) {
  return {
      .x_axis = RowOf(matrix, 0),
      .y_axis = RowOf(matrix, 1),
      .z_axis = RowOf(matrix, 2),
      .translation = RowOf(matrix, 3),
  };
}

// When the frame is within `shutter`, from 0 when it opens to 1 when it
// closes, at its nearer end for a frame outside it; 0 for a shutter that
// spans no interval.
float FrameTime(const Shutter& shutter) {
  const double span = shutter.close - shutter.open;
  return span > 0.0
             ? static_cast<float>(std::clamp(-shutter.open / span, 0.0, 1.0))
             : 0.0F;
}

// The screen window of `camera` over `image`: its bottom left and size, on
// the plane at a distance of 1 for a perspective camera.
std::array<Point2, 2> ScreenWindowOf(const Camera& camera, const Image& image) {
  const ConformedAperture conformed =
      ConformAperture(camera.aperture, image.resolution,
                      static_cast<double>(image.pixel_aspect_ratio),
                      image.aspect_ratio_conform_policy);
  const double scale =
      camera.projection == Projection::kPerspective && camera.focal_length > 0.0
          ? 1.0 / camera.focal_length
          : 1.0;
  const auto scaled = [scale](double value) {
    return static_cast<float>(scale * value);
  };
  const double width = conformed.aperture.at(0);
  const double height = conformed.aperture.at(1);
  return {
      Point2{
          .x = scaled(camera.aperture_offset.at(0) - (0.5 * width)),
          .y = scaled(camera.aperture_offset.at(1) - (0.5 * height)),
      },
      Point2{.x = scaled(width), .y = scaled(height)},
  };
}

// Gives `model` the lens of `camera`, when it renders depth of field.
void SetLens(const Camera& camera, DepthOfField depth_of_field,
             CameraModel& model) {
  if (camera.projection == Projection::kPerspective &&
      depth_of_field == DepthOfField::kEnabled && camera.f_stop > 0.0 &&
      camera.focus_distance > 0.0) {
    model.lens_radius =
        static_cast<float>(camera.focal_length / (2.0 * camera.f_stop));
    model.focus_distance = static_cast<float>(camera.focus_distance);
  }
}

// A camera's screen window, its bottom left and size, on the plane at a
// distance of 1 for a perspective camera, and its clipping range.
struct ScreenDepth {
  double min_x = 0.0;
  double min_y = 0.0;
  double width = 1.0;
  double height = 1.0;
  double near = 0.0;
  double far = 1.0;
};

// The span of `window`'s clipping range; 1 for an empty one.
double DepthOf(const ScreenDepth& window) {
  return window.far > window.near ? window.far - window.near : 1.0;
}

// The projection to NDC of an orthographic camera: NDC's y runs down from
// its top. x' = (x - min_x) / width, y' = (min_y + height - y) / height,
// z' = (-z - near) / depth, w = 1.
Matrix OrthographicToNdc(const ScreenDepth& window) {
  const double depth = DepthOf(window);
  return {
      1.0 / window.width,
      0.0,
      0.0,
      0.0,
      0.0,
      -1.0 / window.height,
      0.0,
      0.0,
      0.0,
      0.0,
      -1.0 / depth,
      0.0,
      -window.min_x / window.width,
      (window.min_y + window.height) / window.height,
      -window.near / depth,
      1.0,
  };
}

// The projection to NDC of a perspective camera, on the plane at a distance
// of 1: x / -z, y / -z; w = -z, and z' / w runs from 0 at the near plane to
// 1 at the far one.
Matrix PerspectiveToNdc(const ScreenDepth& window) {
  const double depth = DepthOf(window);
  return {
      1.0 / window.width,
      0.0,
      0.0,
      0.0,
      0.0,
      -1.0 / window.height,
      0.0,
      0.0,
      window.min_x / window.width,
      -(window.min_y + window.height) / window.height,
      -window.far / depth,
      -1.0,
      0.0,
      0.0,
      -window.far * window.near / depth,
      0.0,
  };
}

}  // namespace

CameraModel MakeCameraModel(const Camera& camera,
                            const FrameParameters& frame) {
  const std::array<Point2, 2> screen = ScreenWindowOf(camera, frame.image);
  CameraModel model{
      .projection = camera.projection,
      .camera_to_world = {},
      .samples = 1,
      .resolution =
          {
              .x = static_cast<float>(frame.image.resolution.at(0)),
              .y = static_cast<float>(frame.image.resolution.at(1)),
          },
      .screen_min = screen.at(0),
      .screen_size = screen.at(1),
      .lens_radius = 0.0F,
      .focus_distance = 0.0F,
      .near = static_cast<float>(camera.clipping_range.at(0)),
      .far = static_cast<float>(camera.clipping_range.at(1)),
      .motion_blur = frame.motion_blur,
      .frame_time = FrameTime(frame.shutter),
  };
  SetLens(camera, frame.depth_of_field, model);
  const std::span<const Matrix> transforms = camera.camera_to_world.first(
      std::min(camera.camera_to_world.size(), kMaxCameraSamples));
  std::ranges::transform(transforms, model.camera_to_world.begin(), AffineOf);
  model.samples =
      static_cast<std::uint32_t>(std::max<std::size_t>(transforms.size(), 1));
  return model;
}

Matrix CameraToNdc(const Camera& camera, const Image& image) {
  const std::array<Point2, 2> screen = ScreenWindowOf(camera, image);
  const ScreenDepth window{
      .min_x = static_cast<double>(screen.at(0).x),
      .min_y = static_cast<double>(screen.at(0).y),
      .width = static_cast<double>(screen.at(1).x),
      .height = static_cast<double>(screen.at(1).y),
      .near = camera.clipping_range.at(0),
      .far = camera.clipping_range.at(1),
  };
  return camera.projection == Projection::kOrthographic
             ? OrthographicToNdc(window)
             : PerspectiveToNdc(window);
}

}  // namespace umbra::device
