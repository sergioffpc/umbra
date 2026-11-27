// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The shapes of the lights that have one (sphere, rect, disk and cylinder),
// as rays see them in the light's own space: the box that bounds each, and
// where a ray first meets it. These functions are single-source, so that
// the CPU devices and the GPU devices see the same shapes; they are on the
// hot path, and never allocate
// (docs/adr/0003-no-allocations-on-the-hot-path.md).

#ifndef UMBRA_DEVICE_LIGHT_SHAPES_H_
#define UMBRA_DEVICE_LIGHT_SHAPES_H_

#include <cmath>
#include <cstdint>

#include "umbra/core/safety.h"
#include "umbra/device/geometry.h"
#include "umbra/device/single_source.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device {

// The shapes a light can have that rays meet.
enum class LightShapeKind : std::uint8_t {
  // A sphere, around the origin.
  kSphere,
  // A rectangle in the XY plane, centered on the origin.
  kRect,
  // A disk in the XY plane, centered on the origin.
  kDisk,
  // A cylinder along the X axis, centered on the origin, without caps.
  kCylinder,
};

// A light's shape, in meters: a sphere, a disk or a cylinder has a radius,
// a rect a width along X and a height along Y, and a cylinder a length
// along X. The sizes a shape does not have are 0.
struct LightShape {
  LightShapeKind kind = LightShapeKind::kSphere;
  float radius = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
  float length = 0.0F;
};

// The box that bounds a shape.
struct ShapeBounds {
  Vector3 lower;
  Vector3 upper;
};

// A ray in a light's own space, in meters: its direction need not be of unit
// length, and the distances along it are in units of its direction. It meets
// what lies from t_min to t_max, both included.
struct ShapeRay {
  Vector3 origin;
  Vector3 direction;
  float t_min = 0.0F;
  float t_max = 0.0F;
};

// The distance of a ray that meets nothing.
inline constexpr float kNoHit = __builtin_huge_valf();

// Where a ray first meets a shape: the distance along it, kNoHit when it
// meets nothing, and the shape's unit normal there, which points out of a
// sphere or a cylinder, and towards -Z, the side it emits from, on a rect
// or a disk, whichever side the ray comes from.
struct ShapeHit {
  float distance = kNoHit;
  Vector3 normal;
};

// Whether `hit` meets the shape.
UMBRA_HOST_DEVICE [[nodiscard]] inline bool IsHit(const ShapeHit& hit) {
  return hit.distance != kNoHit;
}

namespace light_shapes_internal {

// origin + distance·direction.
UMBRA_HOST_DEVICE inline Vector3 Along(Vector3 origin, Vector3 direction,
                                       float distance) {
  return {
      .x = origin.x + (distance * direction.x),
      .y = origin.y + (distance * direction.y),
      .z = origin.z + (distance * direction.z),
  };
}

UMBRA_HOST_DEVICE inline Vector3 At(const ShapeRay& ray, float distance) {
  return Along(ray.origin, ray.direction, distance);
}

// `distance` when it is within the ray's range, or kNoHit.
UMBRA_HOST_DEVICE inline float InRange(float distance, const ShapeRay& ray) {
  return distance >= ray.t_min && distance <= ray.t_max ? distance : kNoHit;
}

// The first of `near_t` and `far_t`, near_t <= far_t, within the ray's
// range, or kNoHit.
UMBRA_HOST_DEVICE inline float FirstInRange(float near_t, float far_t,
                                            const ShapeRay& ray) {
  const float near_in = InRange(near_t, ray);
  return near_in != kNoHit ? near_in : InRange(far_t, ray);
}

// The distances, near first, at which a ray o + t·d meets the points at
// `radius` from an axis or a center, given d·d, o·d, o·o and the square of
// the closest the ray comes to it. That closest distance keeps the
// discriminant accurate for rays far from the shape (Haines et al.,
// "Precision Improvements for Ray/Sphere Intersection", Ray Tracing Gems,
// 2019). Both are kNoHit when the ray misses it.
struct Roots {
  float near_t = kNoHit;
  float far_t = kNoHit;
};

UMBRA_HOST_DEVICE inline Roots QuadricRoots(float dir_dir, float org_dir,
                                            float org_org, float closest_sq,
                                            float radius) {
  const float radius_sq = radius * radius;
  const float discriminant = dir_dir * (radius_sq - closest_sq);
  if (dir_dir == 0.0F || discriminant < 0.0F) {
    return {};
  }
  const float half_b = -org_dir;
  const float root = half_b + std::copysign(std::sqrt(discriminant), half_b);
  if (root == 0.0F) {
    return {};
  }
  const float first = (org_org - radius_sq) / root;
  const float second = root / dir_dir;
  return first <= second ? Roots{.near_t = first, .far_t = second}
                         : Roots{.near_t = second, .far_t = first};
}

UMBRA_HOST_DEVICE inline ShapeHit IntersectSphere(const ShapeRay& ray,
                                                  float radius) {
  const float dir_dir = Dot(ray.direction, ray.direction);
  const float org_dir = Dot(ray.origin, ray.direction);
  const Vector3 closest =
      dir_dir == 0.0F ? ray.origin : At(ray, -org_dir / dir_dir);
  const Roots roots =
      QuadricRoots(dir_dir, org_dir, Dot(ray.origin, ray.origin),
                   Dot(closest, closest), radius);
  const float distance = FirstInRange(roots.near_t, roots.far_t, ray);
  if (distance == kNoHit) {
    return {};
  }
  const Vector3 point = At(ray, distance);
  return {
      .distance = distance,
      .normal =
          {
              .x = point.x / radius,
              .y = point.y / radius,
              .z = point.z / radius,
          },
  };
}

// Where a ray meets the XY plane, within its range, or kNoHit; a ray
// parallel to it never does.
UMBRA_HOST_DEVICE inline float PlaneDistance(const ShapeRay& ray) {
  if (ray.direction.z == 0.0F) {
    return kNoHit;
  }
  const float distance = -ray.origin.z / ray.direction.z;
  return FirstInRange(distance, distance, ray);
}

UMBRA_HOST_DEVICE inline ShapeHit IntersectRect(const ShapeRay& ray,
                                                float width, float height) {
  const float distance = PlaneDistance(ray);
  if (distance == kNoHit) {
    return {};
  }
  const Vector3 point = At(ray, distance);
  if (std::fabs(point.x) > 0.5F * width || std::fabs(point.y) > 0.5F * height) {
    return {};
  }
  return {.distance = distance, .normal = {.x = 0.0F, .y = 0.0F, .z = -1.0F}};
}

UMBRA_HOST_DEVICE inline ShapeHit IntersectDisk(const ShapeRay& ray,
                                                float radius) {
  const float distance = PlaneDistance(ray);
  if (distance == kNoHit) {
    return {};
  }
  const Vector3 point = At(ray, distance);
  if ((point.x * point.x) + (point.y * point.y) > radius * radius) {
    return {};
  }
  return {.distance = distance, .normal = {.x = 0.0F, .y = 0.0F, .z = -1.0F}};
}

// The cylinder's hit at `distance`, when it lies within its length.
UMBRA_HOST_DEVICE inline ShapeHit CylinderHitAt(const ShapeRay& ray,
                                                float distance,
                                                const LightShape& shape) {
  if (distance == kNoHit) {
    return {};
  }
  const Vector3 point = At(ray, distance);
  if (std::fabs(point.x) > 0.5F * shape.length) {
    return {};
  }
  return {
      .distance = distance,
      .normal =
          {
              .x = 0.0F,
              .y = point.y / shape.radius,
              .z = point.z / shape.radius,
          },
  };
}

UMBRA_HOST_DEVICE inline ShapeHit IntersectCylinder(const ShapeRay& ray,
                                                    const LightShape& shape) {
  // Across the axis, the cylinder is a circle in YZ.
  const Vector3 origin{.x = 0.0F, .y = ray.origin.y, .z = ray.origin.z};
  const Vector3 direction{
      .x = 0.0F,
      .y = ray.direction.y,
      .z = ray.direction.z,
  };
  const float dir_dir = Dot(direction, direction);
  const float org_dir = Dot(origin, direction);
  const Vector3 closest =
      Along(origin, direction, dir_dir == 0.0F ? 0.0F : -org_dir / dir_dir);
  const Roots roots = QuadricRoots(dir_dir, org_dir, Dot(origin, origin),
                                   Dot(closest, closest), shape.radius);
  // The nearer root may lie beyond the cylinder's ends, and the farther one
  // on its inside.
  const ShapeHit near_hit =
      CylinderHitAt(ray, InRange(roots.near_t, ray), shape);
  return IsHit(near_hit) ? near_hit
                         : CylinderHitAt(ray, InRange(roots.far_t, ray), shape);
}

}  // namespace light_shapes_internal

// The box that bounds `shape`.
UMBRA_HOST_DEVICE [[nodiscard]] inline ShapeBounds BoundsOf(
    const LightShape& shape) {
  switch (shape.kind) {
    case LightShapeKind::kSphere:
      return {
          .lower = {.x = -shape.radius, .y = -shape.radius, .z = -shape.radius},
          .upper = {.x = shape.radius, .y = shape.radius, .z = shape.radius},
      };
    case LightShapeKind::kRect:
      return {
          .lower = {.x = -0.5F * shape.width, .y = -0.5F * shape.height},
          .upper = {.x = 0.5F * shape.width, .y = 0.5F * shape.height},
      };
    case LightShapeKind::kDisk:
      return {
          .lower = {.x = -shape.radius, .y = -shape.radius},
          .upper = {.x = shape.radius, .y = shape.radius},
      };
    case LightShapeKind::kCylinder:
      return {
          .lower =
              {
                  .x = -0.5F * shape.length,
                  .y = -shape.radius,
                  .z = -shape.radius,
              },
          .upper =
              {
                  .x = 0.5F * shape.length,
                  .y = shape.radius,
                  .z = shape.radius,
              },
      };
  }
  return {};
}

// Where `ray` first meets `shape`, within its range.
UMBRA_HOST_DEVICE [[nodiscard]] inline ShapeHit Intersect(
    const LightShape& shape, const ShapeRay& ray) {
  switch (shape.kind) {
    case LightShapeKind::kSphere:
      return light_shapes_internal::IntersectSphere(ray, shape.radius);
    case LightShapeKind::kRect:
      return light_shapes_internal::IntersectRect(ray, shape.width,
                                                  shape.height);
    case LightShapeKind::kDisk:
      return light_shapes_internal::IntersectDisk(ray, shape.radius);
    case LightShapeKind::kCylinder:
      return light_shapes_internal::IntersectCylinder(ray, shape);
  }
  return {};
}

}  // namespace umbra::device

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_LIGHT_SHAPES_H_
