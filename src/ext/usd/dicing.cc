// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "dicing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>

#include "umbra/device/render_data.h"

namespace umbra::ext::usd::internal::subdivision {
namespace {

// The bounds of a set of points: its least and greatest coordinates.
struct Box {
  Point3 min{
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::max(),
      std::numeric_limits<double>::max(),
  };
  Point3 max{
      std::numeric_limits<double>::lowest(),
      std::numeric_limits<double>::lowest(),
      std::numeric_limits<double>::lowest(),
  };

  void Extend(const Point3& point) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      min.at(axis) = std::min(min.at(axis), point.at(axis));
      max.at(axis) = std::max(max.at(axis), point.at(axis));
    }
  }
};

// The bounds of `points`, in object space.
Box BoundsOf(std::span<const device::Float3> points) {
  Box box;
  for (const device::Float3& point : points) {
    box.Extend({
        static_cast<double>(point.x),
        static_cast<double>(point.y),
        static_cast<double>(point.z),
    });
  }
  return box;
}

// The corners of `box`.
std::array<Point3, 8> Corners(const Box& box) {
  std::array<Point3, 8> corners{};
  for (std::size_t corner = 0; corner < corners.size(); ++corner) {
    for (std::size_t axis = 0; axis < 3; ++axis) {
      const bool high = ((corner >> axis) & 1U) != 0U;
      corners.at(corner).at(axis) = high ? box.max.at(axis) : box.min.at(axis);
    }
  }
  return corners;
}

// The bounds, in world space, of `box`, in object space, through
// `object_to_world`.
Box WorldBounds(const Box& box, const device::Matrix& object_to_world) {
  Box world;
  for (const Point3& corner : Corners(box)) {
    world.Extend(
        ToWorld(object_to_world, {
                                     .x = static_cast<float>(corner.at(0)),
                                     .y = static_cast<float>(corner.at(1)),
                                     .z = static_cast<float>(corner.at(2)),
                                     .padding = 0.0F,
                                 }));
  }
  return world;
}

// The squared distance from `point` to `box`: 0 inside it.
double SquaredDistance(const Point3& point, const Box& box) {
  double squared = 0.0;
  for (std::size_t axis = 0; axis < 3; ++axis) {
    const double below = box.min.at(axis) - point.at(axis);
    const double above = point.at(axis) - box.max.at(axis);
    const double outside = std::max({below, above, 0.0});
    squared += outside * outside;
  }
  return squared;
}

// The distance between `one` and `other`.
double Distance(const Point3& one, const Point3& other) {
  double squared = 0.0;
  for (std::size_t axis = 0; axis < 3; ++axis) {
    const double step = other.at(axis) - one.at(axis);
    squared += step * step;
  }
  return std::sqrt(squared);
}

// Of every position of the camera in `eyes`, which isn't empty, the one
// closest to `bounds` through `object_to_world`.
Viewpoint ClosestEye(const Box& bounds, const device::Matrix& object_to_world,
                     std::span<const Point3> eyes, double& least) {
  const Box world = WorldBounds(bounds, object_to_world);
  const auto eye =
      std::ranges::min_element(eyes, {}, [&world](const Point3& position) {
        return SquaredDistance(position, world);
      });
  least = SquaredDistance(*eye, world);
  return {.object_to_world = object_to_world, .eye = *eye};
}

}  // namespace

Point3 ToWorld(const device::Matrix& object_to_world,
               const device::Float3& point) {
  const std::array<double, 3> object{
      static_cast<double>(point.x),
      static_cast<double>(point.y),
      static_cast<double>(point.z),
  };
  Point3 world{};
  for (std::size_t column = 0; column < 3; ++column) {
    double sum = object_to_world.at(12 + column);
    for (std::size_t row = 0; row < 3; ++row) {
      sum += object.at(row) * object_to_world.at((row * 4) + column);
    }
    world.at(column) = sum;
  }
  return world;
}

double PixelLength(const ImageScale& scale, const Point3& eye,
                   const Point3& from, const Point3& until) {
  const double length = Distance(from, until);
  if (scale.projection == device::Projection::kOrthographic) {
    return length * scale.pixels_per_unit;
  }
  const double distance = std::max({
      std::min(Distance(eye, from), Distance(eye, until)),
      scale.near,
      std::numeric_limits<double>::min(),
  });
  return length / distance * scale.pixels_per_unit;
}

std::uint32_t EdgeRate(double pixels, double dicing_rate) {
  const double segments = std::ceil(pixels / dicing_rate);
  // NaN and infinite lengths, as a segment at the eye has, are cut the most.
  if (!(segments < static_cast<double>(kMaxEdgeRate))) {
    return kMaxEdgeRate;
  }
  return std::max(static_cast<std::uint32_t>(std::max(segments, 1.0)), 1U);
}

Viewpoint ClosestViewpoint(
    std::span<const device::Float3> points,
    std::span<const std::span<const device::Matrix>> instances,
    std::span<const Point3> eyes) {
  Viewpoint closest;
  if (eyes.empty()) {
    return closest;
  }
  closest.eye = eyes.front();
  const Box bounds = BoundsOf(points);
  double least = std::numeric_limits<double>::infinity();
  for (const device::Matrix& object_to_world : instances | std::views::join) {
    double squared = 0.0;
    const Viewpoint viewpoint =
        ClosestEye(bounds, object_to_world, eyes, squared);
    if (squared < least) {
      least = squared;
      closest = viewpoint;
    }
  }
  return closest;
}

}  // namespace umbra::ext::usd::internal::subdivision
