// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// How finely a subdivision surface is tessellated: until its edges look
// shorter than the dicing rate, in pixels, from the active camera, on the
// instance closest to it over the shutter
// (docs/adr/0016-subdivision-surfaces-are-diced-in-the-sync.md); and the
// view the subdivided meshes are diced for, from that camera and its images.

#ifndef UMBRA_DELEGATE_DICING_H_
#define UMBRA_DELEGATE_DICING_H_

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "host_copy.h"
#include "umbra/core/safety.h"
#include "umbra/device/image.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal::subdivision {

// A point in world space, in meters.
using Point3 = std::array<double, 3>;

// The most segments an edge is cut into, whatever its length on screen: it
// bounds the triangles of a surface that reaches the camera.
inline constexpr std::uint32_t kMaxEdgeRate = 1024;

// How the active camera's image scales a length.
struct ImageScale {
  device::Projection projection = device::Projection::kPerspective;
  // The pixels a length spans: per radian of the camera's view for a
  // perspective camera, per meter for an orthographic one. 0 when there is
  // no camera: every edge is then one segment.
  double pixels_per_unit = 0.0;
  // The distance from the camera, in meters, below which nothing is seen:
  // its near clipping plane.
  double near = 0.0;

  friend bool operator==(const ImageScale&, const ImageScale&) = default;
};

// Where a surface is seen from: the transform of its closest instance, and
// the position of the camera then.
struct Viewpoint {
  // Object-to-world, in meters, row-major, for row vectors.
  device::Matrix object_to_world{
      1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
      0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
  };
  Point3 eye{};
};

// `point`, in object space, in world space through `object_to_world`.
[[nodiscard]] Point3 ToWorld(const device::Matrix& object_to_world,
                             const device::Float3& point);

// The length in pixels of the segment from `from` to `until`, in world space,
// seen from `eye` through `scale`: for a perspective camera, its length
// over its distance from the eye (no less than the near plane), as if it
// faced the camera; for an orthographic one, its length. Foreshortening is
// ignored, so that a surface seen at a grazing angle, or in a reflection,
// is diced as finely as one that faces the camera.
[[nodiscard]] double PixelLength(const ImageScale& scale, const Point3& eye,
                                 const Point3& from, const Point3& until);

// The segments an edge `pixels` long is cut into so that each is at most
// `dicing_rate` pixels long: at least 1, at most kMaxEdgeRate.
[[nodiscard]] std::uint32_t EdgeRate(double pixels, double dicing_rate);

// The viewpoint of the instance closest to the camera over the shutter: of
// every motion sample of every instance's transform, and every position of
// the camera over the shutter, the pair whose distance from the eye to the
// bounds of `points`, in world space, is the least. `instances` gives the
// motion samples of each instance's transform; `eyes` those of the camera's
// position. With no instance or no eye, the identity and the eye at the
// origin.
[[nodiscard]] Viewpoint ClosestViewpoint(
    std::span<const device::Float3> points,
    std::span<const std::span<const device::Matrix>> instances,
    std::span<const Point3> eyes);

}  // namespace umbra::delegate::internal::subdivision

namespace umbra::delegate::internal {

// What subdivided meshes are diced for: the image of the active camera, its
// positions over the shutter, in meters, and the dicing rate, in pixels
// (docs/adr/0016-subdivision-surfaces-are-diced-in-the-sync.md). No
// position when there is no camera: every edge is then one segment.
struct DicingView {
  subdivision::ImageScale scale;
  std::vector<subdivision::Point3> eyes;
  double rate = 1.0;

  friend bool operator==(const DicingView&, const DicingView&) = default;
};

// The view of `camera`, sampled over the shutter, whose images are
// `images`, for a dicing rate of `rate` pixels: its positions, one per
// motion sample of its transform, and how its images scale a length.
[[nodiscard]] DicingView DicingViewOf(const host::Camera& camera,
                                      std::span<const device::Image> images,
                                      double rate);

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_DICING_H_
