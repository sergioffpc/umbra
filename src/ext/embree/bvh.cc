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
#include <cstddef>
#include <expected>
#include <format>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/render_data.h"

namespace umbra::ext::embree::internal {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

// The error of a build of `what` that failed with `code`.
Error BuildError(RTCError code, std::string_view what) {
  return {ErrorCode::kUnavailable, std::format("Embree cannot build {}: {}",
                                               what, rtcGetErrorString(code))};
}

// Embree's error on the calling thread, which reading clears, naming `what`.
Error EmbreeError(RTCDevice device, std::string_view what) {
  return BuildError(rtcGetDeviceError(device), what);
}

// Fails with Embree's error on the calling thread, if any, naming `what`.
Status CheckEmbree(RTCDevice device, std::string_view what) {
  const RTCError code = rtcGetDeviceError(device);
  if (code == RTC_ERROR_NONE) {
    return {};
  }
  return std::unexpected(BuildError(code, what));
}

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
  RTCGeometry geometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
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
  rtcReleaseGeometry(geometry);
}

// The shape a light geometry's user data points to: BuildLight sets it,
// and the shape outlives the geometry.
const device::LightShape& ShapeOf(void* UMBRA_NULLABLE user)
    [[clang::nonblocking]] {
  return *static_cast<const device::LightShape*>(user);
}

void LightBounds(const RTCBoundsFunctionArguments* args) {
  const device::ShapeBounds bounds =
      device::BoundsOf(ShapeOf(args->geometryUserPtr));
  *args->bounds_o = {
      .lower_x = bounds.lower.x,
      .lower_y = bounds.lower.y,
      .lower_z = bounds.lower.z,
      .align0 = 0.0F,
      .upper_x = bounds.upper.x,
      .upper_y = bounds.upper.y,
      .upper_z = bounds.upper.z,
      .align1 = 0.0F,
  };
}

// The lanes of a packet of `count` rays, nonzero where a ray is valid.
std::span<const int> Lanes(const int* valid UMBRA_LIFETIMEBOUND,
                           unsigned int count) [[clang::nonblocking]] {
  // SAFETY: Embree passes a valid mask of one int per ray of the packet.
  UMBRA_UNSAFE_BUFFER_BEGIN
  return {valid, count};
  UMBRA_UNSAFE_BUFFER_END
}

device::ShapeRay ShapeRayOf(const RTCRay& ray) [[clang::nonblocking]] {
  return {
      .origin = {.x = ray.org_x, .y = ray.org_y, .z = ray.org_z},
      .direction = {.x = ray.dir_x, .y = ray.dir_y, .z = ray.dir_z},
      .t_min = ray.tnear,
      .t_max = ray.tfar,
  };
}

// The hit of `shape_hit` on the light primitive `args` intersects.
RTCHit HitOf(const device::ShapeHit& shape_hit,
             const RTCIntersectFunctionNArguments& args)
    [[clang::nonblocking]] {
  RTCHit hit{};
  hit.Ng_x = shape_hit.normal.x;
  hit.Ng_y = shape_hit.normal.y;
  hit.Ng_z = shape_hit.normal.z;
  hit.primID = args.primID;
  hit.geomID = args.geomID;
  hit.instID[0] = args.context->instID[0];
  return hit;
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

// `matrix`, row-major for row vectors, as the floats of Embree's
// column-major matrix for column vectors: the same numbers in order.
std::array<float, 16> EmbreeMatrix(const device::Matrix& matrix) {
  std::array<float, 16> floats{};
  for (std::size_t index = 0; index < floats.size(); ++index) {
    floats.at(index) = static_cast<float>(matrix.at(index));
  }
  return floats;
}

}  // namespace

Scene::~Scene() {
  if (scene_ != nullptr) {
    rtcReleaseScene(scene_);
  }
}

std::expected<Scene, Error> BuildPrototype(RTCDevice device,
                                           const device::Prototype& prototype,
                                           BvhKind kind) {
  Scene scene(rtcNewScene(device));
  if (scene.get() == nullptr) {
    return std::unexpected(EmbreeError(device, prototype.path));
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
  if (auto checked = CheckEmbree(device, prototype.path);
      !checked.has_value()) {
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
  return CheckEmbree(device, prototype.path);
}

std::expected<Scene, Error> BuildLight(RTCDevice device,
                                       device::LightShape& shape) {
  Scene scene(rtcNewScene(device));
  if (scene.get() == nullptr) {
    return std::unexpected(EmbreeError(device, "a light"));
  }
  RTCGeometry geometry = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_USER);
  if (geometry == nullptr) {
    return std::unexpected(EmbreeError(device, "a light"));
  }
  rtcSetGeometryUserPrimitiveCount(geometry, 1);
  rtcSetGeometryUserData(geometry, &shape);
  rtcSetGeometryBoundsFunction(geometry, LightBounds, nullptr);
  rtcSetGeometryIntersectFunction(geometry, LightIntersect);
  rtcSetGeometryOccludedFunction(geometry, LightOccluded);
  rtcCommitGeometry(geometry);
  rtcAttachGeometryByID(scene.get(), geometry, 0);
  rtcReleaseGeometry(geometry);
  rtcCommitScene(scene.get());
  if (auto checked = CheckEmbree(device, "a light"); !checked.has_value()) {
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
  RTCGeometry geometry = rtcNewGeometry(device_, RTC_GEOMETRY_TYPE_INSTANCE);
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
  rtcReleaseGeometry(geometry);
  placements_.push_back(placement);
}

std::expected<std::pair<Scene, std::vector<Placement>>, Error>
TopLevel::Build() && {
  if (scene_.get() != nullptr) {
    rtcCommitScene(scene_.get());
  }
  if (auto checked = CheckEmbree(device_, "the top level");
      !checked.has_value()) {
    return std::unexpected(std::move(checked.error()));
  }
  return std::pair(std::move(scene_), std::move(placements_));
}

}  // namespace umbra::ext::embree::internal
