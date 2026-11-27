// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/image.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace umbra::device {
namespace {

// The first and the end, excluded, of the pixels along an axis whose
// centers lie from `low` to `high` along it, both in
// pixels: the center of pixel i is at i + 0.5.
std::array<std::int32_t, 2> PixelRange(double low, double high) {
  return {
      static_cast<std::int32_t>(std::ceil(low - 0.5)),
      static_cast<std::int32_t>(std::floor(high - 0.5)) + 1,
  };
}

}  // namespace

std::array<double, 2> PixelCenterNdc(
    const std::array<std::int32_t, 2>& resolution, std::int32_t column,
    std::int32_t row) {
  const double width = resolution.at(0);
  const double height = resolution.at(1);
  return {
      (static_cast<double>(column) + 0.5) / width,
      1.0 - ((static_cast<double>(row) + 0.5) / height),
  };
}

PixelWindow PixelWindowOf(const std::array<std::int32_t, 2>& resolution,
                          const std::array<float, 4>& data_window_ndc) {
  const double width = resolution.at(0);
  const double height = resolution.at(1);
  const auto x_min = static_cast<double>(data_window_ndc.at(0));
  const auto y_min = static_cast<double>(data_window_ndc.at(1));
  const auto x_max = static_cast<double>(data_window_ndc.at(2));
  const auto y_max = static_cast<double>(data_window_ndc.at(3));
  const std::array<std::int32_t, 2> columns =
      PixelRange(x_min * width, x_max * width);
  // Rows count down from the top: NDC's y is flipped.
  const std::array<std::int32_t, 2> rows =
      PixelRange((1.0 - y_max) * height, (1.0 - y_min) * height);
  return {
      .x_min = columns.at(0),
      .y_min = rows.at(0),
      .x_end = columns.at(1),
      .y_end = rows.at(1),
  };
}

ConformedAperture ConformAperture(const std::array<double, 2>& aperture,
                                  const std::array<std::int32_t, 2>& resolution,
                                  double pixel_aspect_ratio,
                                  AspectRatioConformPolicy policy) {
  const double width = aperture.at(0);
  const double height = aperture.at(1);
  const double image_aspect = static_cast<double>(resolution.at(0)) *
                              pixel_aspect_ratio /
                              static_cast<double>(resolution.at(1));
  const double aperture_aspect = width / height;
  const ConformedAperture adjust_width{
      .aperture = {height * image_aspect, height},
      .pixel_aspect_ratio = pixel_aspect_ratio,
  };
  const ConformedAperture adjust_height{
      .aperture = {width, width / image_aspect},
      .pixel_aspect_ratio = pixel_aspect_ratio,
  };
  const bool wider_image = image_aspect > aperture_aspect;
  switch (policy) {
    case AspectRatioConformPolicy::kExpandAperture:
      return wider_image ? adjust_width : adjust_height;
    case AspectRatioConformPolicy::kCropAperture:
      return wider_image ? adjust_height : adjust_width;
    case AspectRatioConformPolicy::kAdjustApertureWidth:
      return adjust_width;
    case AspectRatioConformPolicy::kAdjustApertureHeight:
      return adjust_height;
    case AspectRatioConformPolicy::kAdjustPixelAspectRatio:
      break;
  }
  return {
      .aperture = aperture,
      .pixel_aspect_ratio = aperture_aspect *
                            static_cast<double>(resolution.at(1)) /
                            static_cast<double>(resolution.at(0)),
  };
}

}  // namespace umbra::device
