// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The points, vectors and transforms of single-source code, in float: what
// the kernels of every device compute with. These functions never allocate
// (docs/adr/0003-no-allocations-on-the-hot-path.md).

#ifndef UMBRA_DEVICE_GEOMETRY_H_
#define UMBRA_DEVICE_GEOMETRY_H_

#include <cmath>

#include "umbra/core/safety.h"
#include "umbra/device/single_source.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device {

// A point or a vector of two dimensions.
struct Point2 {
  float x = 0.0F;
  float y = 0.0F;
};

// A point or a vector, in meters.
struct Vector3 {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
};

UMBRA_HOST_DEVICE [[nodiscard]] inline Vector3 operator+(
    Vector3 lhs, Vector3 rhs) UMBRA_NONBLOCKING {
  return {.x = lhs.x + rhs.x, .y = lhs.y + rhs.y, .z = lhs.z + rhs.z};
}

UMBRA_HOST_DEVICE [[nodiscard]] inline Vector3 operator-(
    Vector3 lhs, Vector3 rhs) UMBRA_NONBLOCKING {
  return {.x = lhs.x - rhs.x, .y = lhs.y - rhs.y, .z = lhs.z - rhs.z};
}

UMBRA_HOST_DEVICE [[nodiscard]] inline Vector3 operator*(
    float scale, Vector3 vector) UMBRA_NONBLOCKING {
  return {.x = scale * vector.x, .y = scale * vector.y, .z = scale * vector.z};
}

UMBRA_HOST_DEVICE [[nodiscard]] inline float Dot(Vector3 lhs, Vector3 rhs)
    UMBRA_NONBLOCKING {
  return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z);
}

UMBRA_HOST_DEVICE [[nodiscard]] inline Vector3 Cross(Vector3 lhs, Vector3 rhs)
    UMBRA_NONBLOCKING {
  return {
      .x = (lhs.y * rhs.z) - (lhs.z * rhs.y),
      .y = (lhs.z * rhs.x) - (lhs.x * rhs.z),
      .z = (lhs.x * rhs.y) - (lhs.y * rhs.x),
  };
}

UMBRA_HOST_DEVICE [[nodiscard]] inline float Length(Vector3 vector)
    UMBRA_NONBLOCKING {
  return std::sqrt(Dot(vector, vector));
}

// `vector` at unit length; a zero vector stays zero.
UMBRA_HOST_DEVICE [[nodiscard]] inline Vector3 Normalize(Vector3 vector)
    UMBRA_NONBLOCKING {
  const float length = Length(vector);
  return length > 0.0F ? (1.0F / length) * vector : vector;
}

// An affine transform, for row vectors, as Matrix is: a point p maps to
// p·M. Its rows are the images of the X, Y and Z axes, then the
// translation; the last column of Matrix, (0, 0, 0, 1), is left out.
struct Affine {
  Vector3 x_axis{.x = 1.0F};
  Vector3 y_axis{.y = 1.0F};
  Vector3 z_axis{.z = 1.0F};
  Vector3 translation;
};

// The image of the vector `vector` by `transform`: without its translation.
UMBRA_HOST_DEVICE [[nodiscard]] inline Vector3 TransformVector(
    const Affine& transform, Vector3 vector) UMBRA_NONBLOCKING {
  return (vector.x * transform.x_axis) + (vector.y * transform.y_axis) +
         (vector.z * transform.z_axis);
}

// The image of the point `point` by `transform`.
UMBRA_HOST_DEVICE [[nodiscard]] inline Vector3 TransformPoint(
    const Affine& transform, Vector3 point) UMBRA_NONBLOCKING {
  return TransformVector(transform, point) + transform.translation;
}

// The vector between `start`, at 0, and `end`, at 1, at `weight`.
UMBRA_HOST_DEVICE [[nodiscard]] inline Vector3 Lerp(
    Vector3 start, Vector3 end, float weight) UMBRA_NONBLOCKING {
  return start + (weight * (end - start));
}

// The transform between `start`, at 0, and `end`, at 1, at `weight`, each
// of its numbers interpolated linearly.
UMBRA_HOST_DEVICE [[nodiscard]] inline Affine Lerp(
    const Affine& start, const Affine& end, float weight) UMBRA_NONBLOCKING {
  return {
      .x_axis = Lerp(start.x_axis, end.x_axis, weight),
      .y_axis = Lerp(start.y_axis, end.y_axis, weight),
      .z_axis = Lerp(start.z_axis, end.z_axis, weight),
      .translation = Lerp(start.translation, end.translation, weight),
  };
}

}  // namespace umbra::device

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_GEOMETRY_H_
