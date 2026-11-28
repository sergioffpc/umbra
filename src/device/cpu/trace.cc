// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "trace.h"

#include <embree4/rtcore_common.h>
#include <embree4/rtcore_device.h>
#include <embree4/rtcore_ray.h>
#include <embree4/rtcore_scene.h>

#include <optional>
#include <span>

#include "bvh.h"
#include "umbra/core/safety.h"
#include "umbra/device/cpu/cpu_device.h"
#include "umbra/device/geometry.h"
#include "umbra/device/render_data.h"
#include "umbra/ext/embree/nonblocking.h"

namespace umbra::device::cpu::internal {
namespace {

RTCRayHit RayHitOf(const Ray& ray) [[clang::nonblocking]] {
  RTCRayHit ray_hit{};
  ray_hit.ray.org_x = ray.origin.x;
  ray_hit.ray.org_y = ray.origin.y;
  ray_hit.ray.org_z = ray.origin.z;
  ray_hit.ray.dir_x = ray.direction.x;
  ray_hit.ray.dir_y = ray.direction.y;
  ray_hit.ray.dir_z = ray.direction.z;
  ray_hit.ray.tnear = ray.t_min;
  ray_hit.ray.tfar = ray.t_max;
  ray_hit.ray.time = ray.time;
  ray_hit.ray.mask = ~0U;
  ray_hit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
  ray_hit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
  return ray_hit;
}

// A transform as Embree gives it in RTC_FORMAT_FLOAT3X4_COLUMN_MAJOR: 12
// floats, the columns one after another.
struct Columns {
  device::Vector3 x_axis;
  device::Vector3 y_axis;
  device::Vector3 z_axis;
  device::Vector3 translation;
};
static_assert(sizeof(Columns) == 12 * sizeof(float));

// The geometric normal `normal`, in the object space of the instance a ray
// met at `time` in `top`, in world space, of unit length, facing
// `direction`.
device::Vector3 WorldNormal(RTCScene top, unsigned int instance, float time,
                            device::Vector3 normal, device::Vector3 direction)
    [[clang::nonblocking]] {
  // The instance's object-to-world transform, column-major for column
  // vectors: its columns are the images of the X, Y and Z axes.
  Columns columns;
  rtcGetGeometryTransformFromScene(top, instance, time,
                                   RTC_FORMAT_FLOAT3X4_COLUMN_MAJOR, &columns);
  const device::Vector3& x_axis = columns.x_axis;
  const device::Vector3& y_axis = columns.y_axis;
  const device::Vector3& z_axis = columns.z_axis;
  // Normals transform by the inverse transpose, which the cofactors give
  // up to the determinant: its sign is lost as the normal faces the ray.
  const device::Vector3 world =
      device::Normalize((normal.x * device::Cross(y_axis, z_axis)) +
                        (normal.y * device::Cross(z_axis, x_axis)) +
                        (normal.z * device::Cross(x_axis, y_axis)));
  return device::Dot(world, direction) > 0.0F ? -1.0F * world : world;
}

}  // namespace

std::optional<Hit> Intersect(RTCScene UMBRA_NULLABLE top,
                             std::span<const Placement> placements,
                             const Ray& ray) [[clang::nonblocking]] {
  if (top == nullptr) {
    return std::nullopt;
  }
  RTCRayHit ray_hit = RayHitOf(ray);
  rtcIntersect1(top, &ray_hit, nullptr);
  if (ray_hit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
    return std::nullopt;
  }
  // at() may throw: the hardened span checks the bounds and aborts.
  const Placement& placement =
      placements.subspan(ray_hit.hit.instID[0], 1).front();
  return Hit{
      .kind = placement.kind,
      .id = placement.id,
      .instance = placement.instance,
      .primitive =
          placement.kind == HitKind::kPrototype ? ray_hit.hit.primID : 0U,
      .distance = ray_hit.ray.tfar,
      .normal = WorldNormal(
          top, ray_hit.hit.instID[0], ray.time,
          {.x = ray_hit.hit.Ng_x, .y = ray_hit.hit.Ng_y, .z = ray_hit.hit.Ng_z},
          {.x = ray.direction.x, .y = ray.direction.y, .z = ray.direction.z}),
  };
}

}  // namespace umbra::device::cpu::internal
