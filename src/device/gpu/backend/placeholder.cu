// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The placeholder integrator's OptiX programs. The ray generation program
// renders each pixel with the single-source placeholder pixel and writes it
// into the film with the single-source film, as the CPU devices do. The
// other programs find the first hit: a triangle's, or a light shape's, which
// an intersection program meets in the light's own space.

#include <optix.h>

#include <cstdint>

#include "launch.h"
#include "umbra/device/camera_rays.h"
#include "umbra/device/film.h"
#include "umbra/device/geometry.h"
#include "umbra/device/light_shapes.h"

extern "C" {
__constant__ umbra::device::gpu::backend::LaunchParams params;
}

namespace umbra::device::gpu::backend {
namespace {

using device::Vector3;

__device__ float3 Float3Of(Vector3 vector) {
  return make_float3(vector.x, vector.y, vector.z);
}

__device__ Vector3 VectorOf(float3 vector) {
  return {.x = vector.x, .y = vector.y, .z = vector.z};
}

// The first hit of `ray` in the IAS.
__device__ device::FirstHit TraceRay(const device::CameraRay& ray) {
  unsigned int distance = __float_as_uint(device::kNoHit);
  unsigned int normal_x = 0;
  unsigned int normal_y = 0;
  unsigned int normal_z = 0;
  optixTrace(params.top, Float3Of(ray.origin), Float3Of(ray.direction),
             ray.t_min, ray.t_max, ray.time, OptixVisibilityMask(0xFF),
             OPTIX_RAY_FLAG_DISABLE_ANYHIT, 0, 1, 0, distance, normal_x,
             normal_y, normal_z);
  return {
      .distance = __uint_as_float(distance),
      .normal =
          {
              .x = __uint_as_float(normal_x),
              .y = __uint_as_float(normal_y),
              .z = __uint_as_float(normal_z),
          },
  };
}

// Reports the hit at the current distance, whose normal in object space is
// `normal`: in world space, of unit length, facing the ray.
__device__ void ReportHit(float3 normal) {
  const Vector3 world = device::Normalize(
      VectorOf(optixTransformNormalFromObjectToWorldSpace(normal)));
  const Vector3 faced =
      device::Dot(world, VectorOf(optixGetWorldRayDirection())) > 0.0F
          ? -1.0F * world
          : world;
  optixSetPayload_0(__float_as_uint(optixGetRayTmax()));
  optixSetPayload_1(__float_as_uint(faced.x));
  optixSetPayload_2(__float_as_uint(faced.y));
  optixSetPayload_3(__float_as_uint(faced.z));
}

}  // namespace
}  // namespace umbra::device::gpu::backend

// The programs, by the names the pipeline looks them up with.
// NOLINTBEGIN(readability-identifier-naming): OptiX's semantic prefixes.

extern "C" __global__ void __raygen__placeholder() {
  const uint3 index = optixGetLaunchIndex();
  const auto* const tiles =
      reinterpret_cast<const umbra::device::PixelWindow*>(params.tiles);
  const umbra::device::PixelWindow& tile = tiles[index.z];
  const std::int32_t column = tile.x_min + static_cast<std::int32_t>(index.x);
  const std::int32_t row = tile.y_min + static_cast<std::int32_t>(index.y);
  if (column >= tile.x_end || row >= tile.y_end) {
    return;
  }
  const umbra::device::PixelSampling sampling{
      .camera = &params.camera,
      .sampler_cache = reinterpret_cast<const void*>(params.sampler_cache),
      .samples_per_pixel = params.samples_per_pixel,
  };
  // A lambda, which the program calls directly, as OptiX requires.
  const auto trace = [](const umbra::device::CameraRay& ray) {
    return umbra::device::gpu::backend::TraceRay(ray);
  };
  umbra::device::WritePixel(
      umbra::device::PlaceholderPixel(sampling, column, row, trace),
      umbra::device::PixelIndex(params.window, column, row), params.layout,
      {reinterpret_cast<float*>(params.film), params.layout.floats});
}

extern "C" __global__ void __miss__placeholder() {}

extern "C" __global__ void __closesthit__triangle() {
  float3 vertices[3];
  optixGetTriangleVertexData(vertices);
  const umbra::device::Vector3 first{
      .x = vertices[1].x - vertices[0].x,
      .y = vertices[1].y - vertices[0].y,
      .z = vertices[1].z - vertices[0].z,
  };
  const umbra::device::Vector3 second{
      .x = vertices[2].x - vertices[0].x,
      .y = vertices[2].y - vertices[0].y,
      .z = vertices[2].z - vertices[0].z,
  };
  umbra::device::gpu::backend::ReportHit(umbra::device::gpu::backend::Float3Of(
      umbra::device::Cross(first, second)));
}

extern "C" __global__ void __intersection__light_shape() {
  const auto* const shapes =
      reinterpret_cast<const umbra::device::LightShape*>(params.light_shapes);
  const umbra::device::ShapeRay ray{
      .origin =
          umbra::device::gpu::backend::VectorOf(optixGetObjectRayOrigin()),
      .direction =
          umbra::device::gpu::backend::VectorOf(optixGetObjectRayDirection()),
      .t_min = optixGetRayTmin(),
      .t_max = optixGetRayTmax(),
  };
  const umbra::device::ShapeHit hit =
      umbra::device::Intersect(shapes[optixGetInstanceId()], ray);
  if (umbra::device::IsHit(hit)) {
    optixReportIntersection(hit.distance, 0, __float_as_uint(hit.normal.x),
                            __float_as_uint(hit.normal.y),
                            __float_as_uint(hit.normal.z));
  }
}

extern "C" __global__ void __closesthit__light() {
  umbra::device::gpu::backend::ReportHit(
      make_float3(__uint_as_float(optixGetAttribute_0()),
                  __uint_as_float(optixGetAttribute_1()),
                  __uint_as_float(optixGetAttribute_2())));
}

// NOLINTEND(readability-identifier-naming)
