// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The image a render product holds, as its render settings frame it: its
// resolution, the pixels of its data window, the quantities its render vars
// hold, and how the camera's aperture is conformed to the image when their
// aspect ratios differ, as UsdRender defines it.
//
// Pixels are numbered from the top left, x to the right and y down, as in an
// OpenEXR file. NDC, as UsdRender defines it, runs from (0, 0) at the bottom
// left of the image to (1, 1) at its top right: y is flipped between them.

#ifndef UMBRA_DEVICE_IMAGE_H_
#define UMBRA_DEVICE_IMAGE_H_

#include <array>
#include <cstdint>

#include "umbra/core/safety.h"
#include "umbra/device/single_source.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device {

// How the aspect ratio of the aperture is conformed to that of the image
// when they differ, as UsdRender defines it.
enum class AspectRatioConformPolicy : std::uint8_t {
  // The aperture grows in width or in height until it matches.
  kExpandAperture,
  // The aperture shrinks in width or in height until it matches.
  kCropAperture,
  // The aperture's width changes to match.
  kAdjustApertureWidth,
  // The aperture's height changes to match.
  kAdjustApertureHeight,
  // The aperture is kept, and the pixels' aspect ratio changes to match.
  kAdjustPixelAspectRatio,
};

// The quantity a render var of a product holds per pixel.
enum class VarSource : std::uint8_t {
  // The radiance, RGBA.
  kColor,
  // The distance along the camera ray to the first hit, in meters.
  kDepth,
  // The normal of the first hit, in world space.
  kNormal,
  // The diffuse reflectance of the first hit.
  kAlbedo,
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

// The pixels of a data window, as a half-open range of pixel coordinates:
// x from x_min to x_end, y from y_min to y_end, excluded. With overscan,
// its coordinates go below 0 or beyond the resolution.
struct PixelWindow {
  std::int32_t x_min = 0;
  std::int32_t y_min = 0;
  std::int32_t x_end = 0;
  std::int32_t y_end = 0;

  // Whether it has no pixel.
  UMBRA_HOST_DEVICE [[nodiscard]] bool IsEmpty() const UMBRA_NONBLOCKING {
    return x_end <= x_min || y_end <= y_min;
  }
  UMBRA_HOST_DEVICE [[nodiscard]] std::int32_t width() const UMBRA_NONBLOCKING {
    return x_end - x_min;
  }
  UMBRA_HOST_DEVICE [[nodiscard]] std::int32_t height() const
      UMBRA_NONBLOCKING {
    return y_end - y_min;
  }

  friend bool operator==(const PixelWindow&, const PixelWindow&) = default;
};

// The image of a render product, as a device renders it.
struct Image {
  // Its size in pixels, width then height.
  std::array<std::int32_t, 2> resolution{};
  // The pixels it renders: its data window.
  PixelWindow window;
  // The width of a pixel over its height, before the aperture is conformed.
  float pixel_aspect_ratio = 1.0F;
  AspectRatioConformPolicy aspect_ratio_conform_policy =
      AspectRatioConformPolicy::kExpandAperture;
};

// The NDC of the center of the pixel at `column` and `row` of an image of
// `resolution` pixels, width then height.
[[nodiscard]] std::array<double, 2> PixelCenterNdc(
    const std::array<std::int32_t, 2>& resolution, std::int32_t column,
    std::int32_t row);

// The pixels of an image of `resolution` whose centers lie in
// `data_window_ndc` (xmin, ymin, xmax, ymax, in NDC), its edges included:
// beyond the image where the window overscans it. Empty when no center lies
// in it.
[[nodiscard]] PixelWindow PixelWindowOf(
    const std::array<std::int32_t, 2>& resolution,
    const std::array<float, 4>& data_window_ndc);

// The aperture a camera sees an image through once conformed, and the
// aspect ratio of the image's pixels.
struct ConformedAperture {
  // Its width and height, in the units of the camera's aperture.
  std::array<double, 2> aperture{};
  // The width of a pixel over its height.
  double pixel_aspect_ratio = 1.0;
};

// Conforms `aperture`, width then height, to an image of `resolution`
// pixels whose aspect ratio is `pixel_aspect_ratio`, as `policy` says.
[[nodiscard]] ConformedAperture ConformAperture(
    const std::array<double, 2>& aperture,
    const std::array<std::int32_t, 2>& resolution, double pixel_aspect_ratio,
    AspectRatioConformPolicy policy);

}  // namespace umbra::device

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_IMAGE_H_
