// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The pixel filter: a Blackman-Harris window, 1.5 pixels wide, separable in
// x and y, which is sampled rather than splatted
// (docs/adr/0014-frames-render-in-tiles-from-one-counter.md). Each sample's
// offset from its pixel's center follows the filter's distribution, by
// filter importance sampling, and the sample counts with weight 1 for its
// pixel alone. These functions are single-source, and never allocate
// (docs/adr/0003-no-allocations-on-the-hot-path.md).

#ifndef UMBRA_DEVICE_PIXEL_FILTER_H_
#define UMBRA_DEVICE_PIXEL_FILTER_H_

#include <cmath>
#include <numbers>

#include "umbra/core/safety.h"
#include "umbra/device/single_source.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device {

// The width of the filter, in pixels: it reaches half of it on each side of
// a pixel's center.
inline constexpr float kPixelFilterWidth = 1.5F;

// An offset from a pixel's center, in pixels, x to the right and y down.
struct FilterOffset {
  float x = 0.0F;
  float y = 0.0F;
};

namespace pixel_filter_internal {

// The coefficients of the four-term Blackman-Harris window.
inline constexpr float kA0 = 0.35875F;
inline constexpr float kA1 = 0.48829F;
inline constexpr float kA2 = 0.14128F;
inline constexpr float kA3 = 0.01168F;
inline constexpr float kTwoPi = 2.0F * std::numbers::pi_v<float>;

// The window at `across`, from 0 to 1 across its width: 1 at its center.
UMBRA_HOST_DEVICE inline float Window(float across) {
  return kA0 - (kA1 * std::cos(kTwoPi * across)) +
         (kA2 * std::cos(2.0F * kTwoPi * across)) -
         (kA3 * std::cos(3.0F * kTwoPi * across));
}

// The fraction of the window's integral from 0 to `across`.
UMBRA_HOST_DEVICE inline float Cumulative(float across) {
  const float integral =
      (kA0 * across) - (kA1 / kTwoPi * std::sin(kTwoPi * across)) +
      (kA2 / (2.0F * kTwoPi) * std::sin(2.0F * kTwoPi * across)) -
      (kA3 / (3.0F * kTwoPi) * std::sin(3.0F * kTwoPi * across));
  return integral / kA0;
}

// `value` within 0 and 1.
UMBRA_HOST_DEVICE inline float Saturate(float value) {
  return std::fmin(std::fmax(value, 0.0F), 1.0F);
}

}  // namespace pixel_filter_internal

// The density of the filter's distribution along one axis, per pixel, at
// `offset` pixels from the center: it integrates to 1 over the filter's
// width, and is 0 beyond it.
UMBRA_HOST_DEVICE [[nodiscard]] inline float PixelFilterDensity(float offset) {
  using pixel_filter_internal::kA0;
  using pixel_filter_internal::Window;
  if (std::fabs(offset) > 0.5F * kPixelFilterWidth) {
    return 0.0F;
  }
  return Window((offset / kPixelFilterWidth) + 0.5F) /
         (kA0 * kPixelFilterWidth);
}

// The offset along one axis, within the filter's width, at which a fraction
// `uniform`, from 0 to 1, of the distribution lies below: the inverse of its
// cumulative distribution, by Newton's method kept within a bracket.
UMBRA_HOST_DEVICE [[nodiscard]] inline float SamplePixelFilter(float uniform) {
  using pixel_filter_internal::Cumulative;
  using pixel_filter_internal::kA0;
  using pixel_filter_internal::Saturate;
  using pixel_filter_internal::Window;
  constexpr int kIterations = 32;
  constexpr float kTolerance = 1e-7F;
  float low = 0.0F;
  float high = 1.0F;
  // Where the sample lies, from 0 to 1 across the filter's width.
  float across = Saturate(uniform);
  for (int iteration = 0; iteration < kIterations; ++iteration) {
    const float error = Cumulative(across) - uniform;
    if (error > 0.0F) {
      high = across;
    } else {
      low = across;
    }
    const float slope = Window(across) / kA0;
    float next = slope > 0.0F ? across - (error / slope) : 0.5F * (low + high);
    if (next <= low || next >= high) {
      next = 0.5F * (low + high);
    }
    const float step = std::fabs(next - across);
    across = next;
    if (step < kTolerance) {
      break;
    }
  }
  return (across - 0.5F) * kPixelFilterWidth;
}

// The offset from a pixel's center of a sample whose uniforms, from 0 to 1,
// are `uniform_x` and `uniform_y`: each axis sampled apart, the filter
// being separable.
UMBRA_HOST_DEVICE [[nodiscard]] inline FilterOffset SamplePixelFilter(
    float uniform_x, float uniform_y) {
  return {.x = SamplePixelFilter(uniform_x), .y = SamplePixelFilter(uniform_y)};
}

}  // namespace umbra::device

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_PIXEL_FILTER_H_
