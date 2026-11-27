// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/camera_rays.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

#include "umbra/device/device.h"
#include "umbra/device/geometry.h"
#include "umbra/device/image.h"
#include "umbra/device/render_data.h"

namespace umbra::device {
namespace {

constexpr Matrix kIdentity{
    1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
};

// The identity, translated by `x` along X.
constexpr Matrix TranslatedX(double x_offset) {
  Matrix matrix = kIdentity;
  matrix.at(12) = x_offset;
  return matrix;
}

constexpr std::array<Matrix, 1> kStill{kIdentity};

// A perspective camera at the origin, looking down -Z, with an aperture
// twice as wide as it is high and a focal length of 1: its screen window,
// at a distance of 1, spans 2 by 1.
Camera PinholeCamera(std::span<const Matrix> camera_to_world = kStill) {
  return {
      .id = CameraId{0},
      .path = "/cam",
      .projection = Projection::kPerspective,
      .camera_to_world = camera_to_world,
      .focal_length = 1.0,
      .aperture = {2.0, 1.0},
      .aperture_offset = {0.0, 0.0},
      .clipping_range = {0.5, 100.0},
      .f_stop = 0.0,
      .focus_distance = 0.0,
      .exposure_scale = 1.0F,
  };
}

// An image of `resolution`, every pixel in its data window.
Image ImageOf(std::array<std::int32_t, 2> resolution,
              AspectRatioConformPolicy policy) {
  return {
      .resolution = resolution,
      .window =
          {
              .x_min = 0,
              .y_min = 0,
              .x_end = resolution.at(0),
              .y_end = resolution.at(1),
          },
      .pixel_aspect_ratio = 1.0F,
      .aspect_ratio_conform_policy = policy,
  };
}

// The frame parameters of `image`, with motion blur and depth of field.
FrameParameters FrameOf(const Image& image) {
  return {
      .shutter = {},
      .camera = CameraId{0},
      .image = image,
      .motion_blur = MotionBlur::kEnabled,
      .depth_of_field = DepthOfField::kEnabled,
      .samples_per_pixel = 1,
      .vars = {},
  };
}

// The samples of a pixel's top left corner, with the lens's center and the
// shutter's middle.
constexpr CameraSamples kTopLeft{
    .filter = {.x = -0.5F, .y = -0.5F},
    .lens = {.x = 0.5F, .y = 0.5F},
    .time = 0.5F,
};
// The samples of a pixel's bottom right corner.
constexpr CameraSamples kBottomRight{
    .filter = {.x = 0.5F, .y = 0.5F},
    .lens = {.x = 0.5F, .y = 0.5F},
    .time = 0.5F,
};

void ExpectVector(Vector3 actual, Vector3 expected) {
  EXPECT_NEAR(actual.x, expected.x, 1e-6F);
  EXPECT_NEAR(actual.y, expected.y, 1e-6F);
  EXPECT_NEAR(actual.z, expected.z, 1e-6F);
}

// The ray of `samples` at pixel (`column`, `row`) of `frame`.
CameraRay RayAt(const Camera& camera, const FrameParameters& frame,
                std::int32_t column, std::int32_t row,
                const CameraSamples& samples) {
  return GenerateRay(MakeCameraModel(camera, frame), column, row, samples);
}

// The corners of an image of `resolution` conformed by `policy` meet the
// corners of a screen window `width` by `height`.
void ExpectCorners(std::array<std::int32_t, 2> resolution,
                   AspectRatioConformPolicy policy, float width, float height) {
  const FrameParameters frame = FrameOf(ImageOf(resolution, policy));
  const CameraRay top_left = RayAt(PinholeCamera(), frame, 0, 0, kTopLeft);
  const CameraRay bottom_right =
      RayAt(PinholeCamera(), frame, resolution.at(0) - 1, resolution.at(1) - 1,
            kBottomRight);

  ExpectVector(top_left.origin, {});
  ExpectVector(top_left.direction,
               Normalize({.x = -0.5F * width, .y = 0.5F * height, .z = -1.0F}));
  ExpectVector(bottom_right.direction,
               Normalize({.x = 0.5F * width, .y = -0.5F * height, .z = -1.0F}));
}

TEST(GenerateRayTest, LooksDownTheAxisAtTheCenter) {
  // The center of a 4x4 image is the corner between pixels 1 and 2.
  const FrameParameters frame =
      FrameOf(ImageOf({4, 4}, AspectRatioConformPolicy::kExpandAperture));

  const CameraRay ray = RayAt(PinholeCamera(), frame, 1, 1, kBottomRight);

  ExpectVector(ray.origin, {});
  ExpectVector(ray.direction, {.z = -1.0F});
}

TEST(GenerateRayTest, ExpandsTheApertureToATallerImage) {
  // A square image: the aperture grows to 2 by 2.
  ExpectCorners({4, 4}, AspectRatioConformPolicy::kExpandAperture, 2.0F, 2.0F);
}

TEST(GenerateRayTest, ExpandsTheApertureToAWiderImage) {
  // An image 4 times as wide as high: the aperture grows to 4 by 1.
  ExpectCorners({8, 2}, AspectRatioConformPolicy::kExpandAperture, 4.0F, 1.0F);
}

TEST(GenerateRayTest, CropsTheApertureToATallerImage) {
  ExpectCorners({4, 4}, AspectRatioConformPolicy::kCropAperture, 1.0F, 1.0F);
}

TEST(GenerateRayTest, CropsTheApertureToAWiderImage) {
  ExpectCorners({8, 2}, AspectRatioConformPolicy::kCropAperture, 2.0F, 0.5F);
}

TEST(GenerateRayTest, AdjustsTheAperturesWidth) {
  ExpectCorners({8, 2}, AspectRatioConformPolicy::kAdjustApertureWidth, 4.0F,
                1.0F);
  ExpectCorners({4, 4}, AspectRatioConformPolicy::kAdjustApertureWidth, 1.0F,
                1.0F);
}

TEST(GenerateRayTest, AdjustsTheAperturesHeight) {
  ExpectCorners({8, 2}, AspectRatioConformPolicy::kAdjustApertureHeight, 2.0F,
                0.5F);
  ExpectCorners({4, 4}, AspectRatioConformPolicy::kAdjustApertureHeight, 2.0F,
                2.0F);
}

TEST(GenerateRayTest, KeepsTheApertureWhenThePixelsAdjust) {
  ExpectCorners({8, 2}, AspectRatioConformPolicy::kAdjustPixelAspectRatio, 2.0F,
                1.0F);
}

TEST(ConformApertureTest, AdjustsThePixelAspectRatio) {
  // An aperture of aspect 2 over 8x2 pixels: each pixel is half as wide as
  // it is high.
  const ConformedAperture conformed =
      ConformAperture({2.0, 1.0}, {8, 2}, 1.0,
                      AspectRatioConformPolicy::kAdjustPixelAspectRatio);

  EXPECT_DOUBLE_EQ(conformed.pixel_aspect_ratio, 0.5);
}

TEST(GenerateRayTest, TakesThePixelAspectRatio) {
  // Pixels twice as wide as high make a 4x4 image twice as wide: expanded,
  // the aperture stays 2 by 1.
  Image image = ImageOf({4, 4}, AspectRatioConformPolicy::kExpandAperture);
  image.pixel_aspect_ratio = 2.0F;

  const CameraRay ray = RayAt(PinholeCamera(), FrameOf(image), 0, 0, kTopLeft);

  ExpectVector(ray.direction, Normalize({.x = -1.0F, .y = 0.5F, .z = -1.0F}));
}

TEST(GenerateRayTest, ShiftsByTheApertureOffset) {
  Camera camera = PinholeCamera();
  camera.aperture_offset = {0.5, 0.25};
  const FrameParameters frame = FrameOf(
      ImageOf({8, 2}, AspectRatioConformPolicy::kAdjustPixelAspectRatio));

  const CameraRay ray = RayAt(camera, frame, 3, 0, kBottomRight);

  ExpectVector(ray.direction, Normalize({.x = 0.5F, .y = 0.25F, .z = -1.0F}));
}

TEST(GenerateRayTest, ClipsToTheClippingRange) {
  const FrameParameters frame =
      FrameOf(ImageOf({4, 4}, AspectRatioConformPolicy::kExpandAperture));

  const CameraRay center = RayAt(PinholeCamera(), frame, 1, 1, kBottomRight);
  const CameraRay corner = RayAt(PinholeCamera(), frame, 0, 0, kTopLeft);

  EXPECT_FLOAT_EQ(center.t_min, 0.5F);
  EXPECT_FLOAT_EQ(center.t_max, 100.0F);
  // The corner's direction is (-1, 1, -1): its planes are sqrt(3) times as
  // far along it.
  EXPECT_FLOAT_EQ(corner.t_min, 0.5F * std::sqrt(3.0F));
  EXPECT_FLOAT_EQ(corner.t_max, 100.0F * std::sqrt(3.0F));
}

TEST(GenerateRayTest, ProjectsOrthographically) {
  Camera camera = PinholeCamera();
  camera.projection = Projection::kOrthographic;
  const FrameParameters frame =
      FrameOf(ImageOf({8, 2}, AspectRatioConformPolicy::kExpandAperture));

  const CameraRay ray = RayAt(camera, frame, 0, 0, kTopLeft);

  ExpectVector(ray.origin, {.x = -2.0F, .y = 0.5F});
  ExpectVector(ray.direction, {.z = -1.0F});
  EXPECT_FLOAT_EQ(ray.t_min, 0.5F);
}

// A camera whose lens, of f-number 1 and focal length 1, is 0.5 in radius,
// focused at 10.
Camera LensCamera() {
  Camera camera = PinholeCamera();
  camera.f_stop = 1.0;
  camera.focus_distance = 10.0;
  return camera;
}

TEST(GenerateRayTest, LeavesTheLensForThePointInFocus) {
  const FrameParameters frame =
      FrameOf(ImageOf({4, 4}, AspectRatioConformPolicy::kExpandAperture));
  // The lens's edge on +X.
  const CameraSamples samples{
      .filter = {.x = 0.5F, .y = 0.5F},
      .lens = {.x = 1.0F, .y = 0.5F},
      .time = 0.5F,
  };

  const CameraRay ray = RayAt(LensCamera(), frame, 1, 1, samples);

  ExpectVector(ray.origin, {.x = 0.5F});
  ExpectVector(ray.direction, Normalize({.x = -0.5F, .z = -10.0F}));
}

TEST(GenerateRayTest, FocusesEveryLensSampleOnOnePoint) {
  const FrameParameters frame =
      FrameOf(ImageOf({4, 4}, AspectRatioConformPolicy::kExpandAperture));

  for (const Point2 lens : {
           Point2{.x = 0.1F, .y = 0.2F},
           Point2{.x = 0.9F, .y = 0.4F},
           Point2{.x = 0.3F, .y = 0.95F},
       }) {
    const CameraSamples samples{
        .filter = {.x = -0.5F, .y = -0.5F},
        .lens = lens,
        .time = 0.5F,
    };
    const CameraRay ray = RayAt(LensCamera(), frame, 0, 0, samples);
    // The pinhole ray of the corner, (-1, 1, -1), meets the plane in focus at
    // (-10, 10, -10).
    const float distance = -10.0F / ray.direction.z;
    ExpectVector(ray.origin + (distance * ray.direction),
                 {.x = -10.0F, .y = 10.0F, .z = -10.0F});
  }
}

TEST(GenerateRayTest, IsAPinholeWithoutDepthOfField) {
  FrameParameters frame =
      FrameOf(ImageOf({4, 4}, AspectRatioConformPolicy::kExpandAperture));
  frame.depth_of_field = DepthOfField::kDisabled;
  const CameraSamples samples{
      .filter = {.x = 0.5F, .y = 0.5F},
      .lens = {.x = 1.0F, .y = 0.5F},
      .time = 0.5F,
  };

  const CameraRay ray = RayAt(LensCamera(), frame, 1, 1, samples);

  ExpectVector(ray.origin, {});
  ExpectVector(ray.direction, {.z = -1.0F});
}

// A camera that moves from x = 0 when the shutter opens to x = 2 when it
// closes.
constexpr std::array<Matrix, 2> kMoving{TranslatedX(0.0), TranslatedX(2.0)};

TEST(GenerateRayTest, MovesWithTheCameraOverTheShutter) {
  FrameParameters frame =
      FrameOf(ImageOf({4, 4}, AspectRatioConformPolicy::kExpandAperture));
  frame.shutter = {.open = -0.5, .close = 0.5};
  CameraSamples samples = kBottomRight;
  samples.time = 0.25F;

  const CameraRay ray = RayAt(PinholeCamera(kMoving), frame, 1, 1, samples);

  ExpectVector(ray.origin, {.x = 0.5F});
  EXPECT_FLOAT_EQ(ray.time, 0.25F);
}

TEST(GenerateRayTest, StaysAtTheFrameWithoutMotionBlur) {
  FrameParameters frame =
      FrameOf(ImageOf({4, 4}, AspectRatioConformPolicy::kExpandAperture));
  // The frame is a quarter of the way into the shutter.
  frame.shutter = {.open = -0.25, .close = 0.75};
  frame.motion_blur = MotionBlur::kDisabled;
  CameraSamples samples = kBottomRight;
  samples.time = 0.9F;

  const CameraRay ray = RayAt(PinholeCamera(kMoving), frame, 1, 1, samples);

  ExpectVector(ray.origin, {.x = 0.5F});
  EXPECT_FLOAT_EQ(ray.time, 0.25F);
}

TEST(GenerateRayTest, GivesWorldDirectionsOfUnitLength) {
  // A camera scaled twice: its clipping range is twice as far in the world.
  Matrix scaled = kIdentity;
  scaled.at(0) = 2.0;
  scaled.at(5) = 2.0;
  scaled.at(10) = 2.0;
  const std::array<Matrix, 1> camera_to_world{scaled};
  const FrameParameters frame =
      FrameOf(ImageOf({4, 4}, AspectRatioConformPolicy::kExpandAperture));

  const CameraRay ray =
      RayAt(PinholeCamera(camera_to_world), frame, 1, 1, kBottomRight);

  ExpectVector(ray.direction, {.z = -1.0F});
  EXPECT_FLOAT_EQ(ray.t_min, 1.0F);
}

}  // namespace
}  // namespace umbra::device
