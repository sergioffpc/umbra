// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The rays a camera casts through the pixels of a render product. The host
// makes a camera model once per product, from the camera and the frame
// parameters; GenerateRay, single-source, turns a pixel and its samples into
// a ray in world space, on the hot path, without allocating
// (docs/adr/0003-no-allocations-on-the-hot-path.md).

#ifndef UMBRA_DEVICE_CAMERA_RAYS_H_
#define UMBRA_DEVICE_CAMERA_RAYS_H_

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numbers>

#include "umbra/core/safety.h"
#include "umbra/device/geometry.h"
#include "umbra/device/image.h"
#include "umbra/device/pixel_filter.h"
#include "umbra/device/render_data.h"
#include "umbra/device/single_source.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device {

// device.h: host code only.
struct FrameParameters;

// The most motion samples of a camera's transform a camera model holds.
inline constexpr std::size_t kMaxCameraSamples = 16;

// A camera as GenerateRay reads it, for one render product.
struct CameraModel {
  Projection projection = Projection::kPerspective;
  // The camera-to-world transform, one per motion sample, equally spaced
  // over the shutter: `samples` of them.
  std::array<Affine, kMaxCameraSamples> camera_to_world{};
  std::uint32_t samples = 1;
  // The resolution of the image: its width and height in pixels.
  Point2 resolution;
  // The bottom left of the screen window, in camera space, and its size: on
  // the plane at a distance of 1 for a perspective camera, in meters for an
  // orthographic one.
  Point2 screen_min;
  Point2 screen_size;
  // The lens's radius, in meters, and the distance in focus: 0 for a
  // pinhole.
  float lens_radius = 0.0F;
  float focus_distance = 0.0F;
  // The clipping range, in meters along the camera's axis.
  float near = 0.0F;
  float far = 0.0F;
  MotionBlur motion_blur = MotionBlur::kEnabled;
  // When the frame is, from 0 when the shutter opens to 1 when it closes:
  // the time of every ray without motion blur.
  float frame_time = 0.0F;
};

// The camera model of `camera` for the product `frame` renders: its aperture
// conformed to the image, and its lens a pinhole without depth of field or
// with an f-number of 0. It keeps the first kMaxCameraSamples motion
// samples.
[[nodiscard]] CameraModel MakeCameraModel(const Camera& camera,
                                          const FrameParameters& frame);

// The projection of `camera`, its aperture conformed to `image`, from its
// own space to the NDC of OpenEXR's worldToNDC: the screen window from
// (0, 0) at its top left to (1, 1) at its bottom right, once divided by w,
// and the clipping range from 0 to 1 in z. Row-major, for row vectors, as
// Matrix is.
[[nodiscard]] Matrix CameraToNdc(const Camera& camera, const Image& image);

// The samples, from 0 to 1, that a camera ray takes.
struct CameraSamples {
  // Its offset from its pixel's center, from the pixel filter.
  FilterOffset filter;
  // Where it leaves the lens.
  Point2 lens;
  // When it is cast, within the shutter.
  float time = 0.0F;
};

// A ray in world space, in meters, with a direction of unit length.
struct CameraRay {
  Vector3 origin;
  Vector3 direction;
  // The distances along it within the clipping range.
  float t_min = 0.0F;
  float t_max = 0.0F;
  // When it is cast, from 0 when the shutter opens to 1 when it closes.
  float time = 0.0F;
};

namespace camera_rays_internal {

// The point of the unit disk at `uniform`, by Shirley and Chiu's concentric
// mapping.
UMBRA_HOST_DEVICE inline Point2 ConcentricDisk(Point2 uniform) {
  constexpr float kQuarterPi = 0.25F * std::numbers::pi_v<float>;
  const float across = (2.0F * uniform.x) - 1.0F;
  const float down = (2.0F * uniform.y) - 1.0F;
  if (across == 0.0F && down == 0.0F) {
    return {};
  }
  const bool wide = std::fabs(across) > std::fabs(down);
  const float radius = wide ? across : down;
  const float angle =
      wide ? kQuarterPi * (down / across)
           : (2.0F * kQuarterPi) - (kQuarterPi * (across / down));
  return {.x = radius * std::cos(angle), .y = radius * std::sin(angle)};
}

// The camera-to-world transform of `model` at `time`, from 0 to 1.
UMBRA_HOST_DEVICE inline Affine CameraToWorld(const CameraModel& model,
                                              float time) {
  if (model.samples <= 1) {
    return model.camera_to_world.front();
  }
  const float position = time * static_cast<float>(model.samples - 1);
  const std::uint32_t last = model.samples - 2;
  const auto floor = static_cast<std::uint32_t>(std::fmax(position, 0.0F));
  const std::uint32_t index = floor < last ? floor : last;
  // Device code has no at(); the index stays below `samples`.
  const auto* const before = std::next(model.camera_to_world.begin(), index);
  return Lerp(*before, *std::next(before),
              position - static_cast<float>(index));
}

// The point of the screen window, in camera space, that the sample of
// `filter` at the pixel at `column` and `row` sees.
UMBRA_HOST_DEVICE inline Point2 ScreenPoint(const CameraModel& model,
                                            std::int32_t column,
                                            std::int32_t row,
                                            FilterOffset filter) {
  // The sample's NDC: y up from the bottom.
  const float ndc_x =
      (static_cast<float>(column) + 0.5F + filter.x) / model.resolution.x;
  const float ndc_y =
      1.0F - ((static_cast<float>(row) + 0.5F + filter.y) / model.resolution.y);
  return {
      .x = model.screen_min.x + (ndc_x * model.screen_size.x),
      .y = model.screen_min.y + (ndc_y * model.screen_size.y),
  };
}

// A ray in camera space, its direction of any length.
struct LocalRay {
  Vector3 origin;
  Vector3 direction;
};

// The ray in camera space through `screen`, from the point of the lens at
// `lens`.
UMBRA_HOST_DEVICE inline LocalRay CameraSpaceRay(const CameraModel& model,
                                                 Point2 screen, Point2 lens) {
  if (model.projection == Projection::kOrthographic) {
    return {
        .origin = {.x = screen.x, .y = screen.y, .z = 0.0F},
        .direction = {.x = 0.0F, .y = 0.0F, .z = -1.0F},
    };
  }
  const Vector3 pinhole{.x = screen.x, .y = screen.y, .z = -1.0F};
  if (model.lens_radius <= 0.0F) {
    return {.origin = {}, .direction = pinhole};
  }
  const Point2 disk = ConcentricDisk(lens);
  const Vector3 origin{
      .x = model.lens_radius * disk.x,
      .y = model.lens_radius * disk.y,
      .z = 0.0F,
  };
  // Towards where the pinhole ray meets the plane in focus.
  return {
      .origin = origin,
      .direction = (model.focus_distance * pinhole) - origin,
  };
}

}  // namespace camera_rays_internal

// The ray `model` casts through the pixel at `column` and `row` of its image,
// with `samples`: through the point the filter offsets from the pixel's
// center, from the point of the lens the lens samples give towards the plane
// in focus, at the time within the shutter the time sample gives, or at the
// frame's without motion blur.
UMBRA_HOST_DEVICE [[nodiscard]] inline CameraRay GenerateRay(
    const CameraModel& model, std::int32_t column, std::int32_t row,
    const CameraSamples& samples) {
  using camera_rays_internal::CameraSpaceRay;
  using camera_rays_internal::CameraToWorld;
  using camera_rays_internal::LocalRay;
  using camera_rays_internal::ScreenPoint;
  const LocalRay local = CameraSpaceRay(
      model, ScreenPoint(model, column, row, samples.filter), samples.lens);
  const Vector3 direction = Normalize(local.direction);
  const float time = model.motion_blur == MotionBlur::kEnabled
                         ? samples.time
                         : model.frame_time;
  const Affine camera_to_world = CameraToWorld(model, time);
  const Vector3 world_direction = TransformVector(camera_to_world, direction);
  const float scale = Length(world_direction);
  // The clipping planes are across the camera's axis, from its origin.
  const float axial = -direction.z;
  return {
      .origin = TransformPoint(camera_to_world, local.origin),
      .direction = (1.0F / scale) * world_direction,
      .t_min = scale * model.near / axial,
      .t_max = scale * model.far / axial,
      .time = time,
  };
}

}  // namespace umbra::device

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_CAMERA_RAYS_H_
