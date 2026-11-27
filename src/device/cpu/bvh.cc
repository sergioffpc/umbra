// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "bvh.h"

#include <embree4/rtcore_buffer.h>
#include <embree4/rtcore_common.h>
#include <embree4/rtcore_device.h>
#include <embree4/rtcore_geometry.h>
#include <embree4/rtcore_ray.h>
#include <embree4/rtcore_scene.h>

#include <array>
#include <expected>
#include <span>
#include <utility>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/render_data.h"
#include "umbra/ext/embree/conversions.h"
#include "umbra/ext/embree/errors.h"
#include "umbra/ext/embree/owners.h"

namespace umbra::device::cpu::internal {
namespace {

using ::umbra::core::Error;
using ::umbra::ext::embree::BuildError;
using ::umbra::ext::embree::CheckBuild;
using ::umbra::ext::embree::EmbreeBounds;
using ::umbra::ext::embree::EmbreeMatrix;
using ::umbra::ext::embree::Geometry;
using ::umbra::ext::embree::HitOf;
using ::umbra::ext::embree::Lanes;
using ::umbra::ext::embree::ShapeRayOf;

// Gives the points of each motion sample of `prototype` to `geometry`, which
// reads them in place.
void SetPoints(RTCGeometry geometry, const device::Prototype& prototype) {
  unsigned int slot = 0;
  for (const std::span<const device::Float3> sample : prototype.points) {
    rtcSetSharedGeometryBuffer(geometry, RTC_BUFFER_TYPE_VERTEX, slot,
                               RTC_FORMAT_FLOAT3, sample.data(), 0,
                               sizeof(device::Float3), sample.size());
    ++slot;
  }
}

// Attaches to `scene` the triangles of `prototype`.
void AttachTriangles(RTCDevice device, RTCScene scene,
                     const device::Prototype& prototype, BvhKind kind) {
  const Geometry owner(rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE));
  RTCGeometry UMBRA_NULLABLE const geometry = owner.get();
  if (geometry == nullptr) {
    return;
  }
  if (kind == BvhKind::kRefittable) {
    rtcSetGeometryBuildQuality(geometry, RTC_BUILD_QUALITY_REFIT);
  }
  rtcSetGeometryTimeStepCount(
      geometry, static_cast<unsigned int>(prototype.points.size()));
  SetPoints(geometry, prototype);
  rtcSetSharedGeometryBuffer(geometry, RTC_BUFFER_TYPE_INDEX, 0,
                             RTC_FORMAT_UINT3, prototype.triangles.data(), 0,
                             sizeof(device::Triangle),
                             prototype.triangles.size());
  rtcCommitGeometry(geometry);
  rtcAttachGeometryByID(scene, geometry, 0);
}

// The shape a light geometry's user data points to: BuildLight sets it,
// and the shape outlives the geometry.
const device::LightShape& ShapeOf(void* UMBRA_NULLABLE user)
    [[clang::nonblocking]] {
  return *static_cast<const device::LightShape*>(user);
}

void LightBounds(const RTCBoundsFunctionArguments* args) {
  *args->bounds_o =
      EmbreeBounds(device::BoundsOf(ShapeOf(args->geometryUserPtr)));
}

void LightIntersect(const RTCIntersectFunctionNArguments* args)
    [[clang::nonblocking]] {
  const device::LightShape& shape = ShapeOf(args->geometryUserPtr);
  RTCRayN* const rays = RTCRayHitN_RayN(args->rayhit, args->N);
  RTCHitN* const hits = RTCRayHitN_HitN(args->rayhit, args->N);
  unsigned int lane = 0;
  for (const int valid : Lanes(args->valid, args->N)) {
    const device::ShapeHit hit =
        valid == 0
            ? device::ShapeHit{}
            : device::Intersect(
                  shape, ShapeRayOf(rtcGetRayFromRayN(rays, args->N, lane)));
    if (device::IsHit(hit)) {
      RTCRayN_tfar(rays, args->N, lane) = hit.distance;
      const RTCHit out = HitOf(hit, *args);
      rtcCopyHitToHitN(hits, &out, args->N, lane);
    }
    ++lane;
  }
}

void LightOccluded(const RTCOccludedFunctionNArguments* args)
    [[clang::nonblocking]] {
  const device::LightShape& shape = ShapeOf(args->geometryUserPtr);
  unsigned int lane = 0;
  for (const int valid : Lanes(args->valid, args->N)) {
    if (valid != 0 &&
        device::IsHit(device::Intersect(
            shape, ShapeRayOf(rtcGetRayFromRayN(args->ray, args->N, lane))))) {
      RTCRayN_tfar(args->ray, args->N, lane) = -__builtin_huge_valf();
    }
    ++lane;
  }
}

// Attaches to `scene` a user geometry of one primitive, `shape`. Fails with
// kUnavailable, with Embree's error, when Embree cannot create it.
Status AttachLight(RTCDevice device, RTCScene scene,
                   device::LightShape& shape) {
  const Geometry owner(rtcNewGeometry(device, RTC_GEOMETRY_TYPE_USER));
  RTCGeometry UMBRA_NULLABLE const geometry = owner.get();
  if (geometry == nullptr) {
    return std::unexpected(BuildError(device, "a light"));
  }
  rtcSetGeometryUserPrimitiveCount(geometry, 1);
  rtcSetGeometryUserData(geometry, &shape);
  rtcSetGeometryBoundsFunction(geometry, LightBounds, nullptr);
  rtcSetGeometryIntersectFunction(geometry, LightIntersect);
  rtcSetGeometryOccludedFunction(geometry, LightOccluded);
  rtcCommitGeometry(geometry);
  rtcAttachGeometryByID(scene, geometry, 0);
  return {};
}

}  // namespace

std::expected<Scene, Error> BuildPrototype(RTCDevice device,
                                           const device::Prototype& prototype,
                                           BvhKind kind) {
  Scene scene(rtcNewScene(device));
  if (scene.get() == nullptr) {
    return std::unexpected(BuildError(device, prototype.path));
  }
  if (kind == BvhKind::kRefittable) {
    // Embree builds each geometry's BVH apart, with its own quality, only
    // at the scene's low quality.
    rtcSetSceneBuildQuality(scene.get(), RTC_BUILD_QUALITY_LOW);
    rtcSetSceneFlags(scene.get(), RTC_SCENE_FLAG_DYNAMIC);
  }
  if (!prototype.triangles.empty()) {
    AttachTriangles(device, scene.get(), prototype, kind);
  }
  rtcCommitScene(scene.get());
  if (auto checked = CheckBuild(device, prototype.path); !checked.has_value()) {
    return std::unexpected(std::move(checked.error()));
  }
  return scene;
}

Status RefitPrototype(RTCDevice device, const Scene& scene,
                      const device::Prototype& prototype) {
  if (prototype.triangles.empty()) {
    return {};
  }
  RTCGeometry geometry = rtcGetGeometry(scene.get(), 0);
  SetPoints(geometry, prototype);
  rtcCommitGeometry(geometry);
  rtcCommitScene(scene.get());
  return CheckBuild(device, prototype.path);
}

std::expected<Scene, Error> BuildLight(RTCDevice device,
                                       device::LightShape& shape) {
  Scene scene(rtcNewScene(device));
  if (scene.get() == nullptr) {
    return std::unexpected(BuildError(device, "a light"));
  }
  if (auto attached = AttachLight(device, scene.get(), shape);
      !attached.has_value()) {
    return std::unexpected(std::move(attached.error()));
  }
  rtcCommitScene(scene.get());
  if (auto checked = CheckBuild(device, "a light"); !checked.has_value()) {
    return std::unexpected(std::move(checked.error()));
  }
  return scene;
}

TopLevel::TopLevel(RTCDevice device)
    : device_(device), scene_(rtcNewScene(device)) {}

void TopLevel::Add(const Scene& bvh, const device::Instance& instance,
                   const Placement& placement) {
  if (scene_.get() == nullptr || bvh.get() == nullptr) {
    return;
  }
  const Geometry owner(rtcNewGeometry(device_, RTC_GEOMETRY_TYPE_INSTANCE));
  RTCGeometry UMBRA_NULLABLE const geometry = owner.get();
  if (geometry == nullptr) {
    return;
  }
  rtcSetGeometryInstancedScene(geometry, bvh.get());
  rtcSetGeometryTimeStepCount(
      geometry, static_cast<unsigned int>(instance.object_to_world.size()));
  unsigned int step = 0;
  for (const device::Matrix& transform : instance.object_to_world) {
    const std::array<float, 16> floats = EmbreeMatrix(transform);
    rtcSetGeometryTransform(geometry, step, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR,
                            floats.data());
    ++step;
  }
  rtcCommitGeometry(geometry);
  rtcAttachGeometryByID(scene_.get(), geometry,
                        static_cast<unsigned int>(placements_.size()));
  placements_.push_back(placement);
}

std::expected<std::pair<Scene, std::vector<Placement>>, Error>
TopLevel::Build() && {
  if (scene_.get() != nullptr) {
    rtcCommitScene(scene_.get());
  }
  if (auto checked = CheckBuild(device_, "the top level");
      !checked.has_value()) {
    return std::unexpected(std::move(checked.error()));
  }
  return std::pair(std::move(scene_), std::move(placements_));
}

}  // namespace umbra::device::cpu::internal
