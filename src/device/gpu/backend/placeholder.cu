// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The placeholder integrator's OptiX programs. The ray generation program
// casts each pixel's camera rays, with the single-source camera model, pixel
// filter and sampler, and writes into the film, as the CPU devices do: the
// color 0.5·direction + 0.5, with alpha 1; the distance to the first hit, in
// meters, or +∞ when a ray meets nothing; the geometric normal of the first
// hit, in world space, facing the ray, or 0; each the mean of the pixel's
// samples. The other programs find the first hit: a triangle's, or a light
// shape's, which an intersection program meets in the light's own space.

#include <optix.h>

#include <cstdint>

#include "launch.h"
#include "umbra/device/camera_rays.h"
#include "umbra/device/geometry.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/sampler.h"

extern "C" {
__constant__ umbra::device::gpu::backend::LaunchParams params;
}

namespace umbra::device::gpu::backend {
namespace {

using device::Vector3;

// The first hit of a ray: its distance, kNoHit when it meets nothing, and
// its normal.
struct Trace {
  float distance = device::kNoHit;
  Vector3 normal;
};

__device__ float3 Float3Of(Vector3 vector) {
  return make_float3(vector.x, vector.y, vector.z);
}

__device__ Vector3 VectorOf(float3 vector) {
  return {.x = vector.x, .y = vector.y, .z = vector.z};
}

// The first hit of `ray` in the IAS.
__device__ Trace TraceRay(const device::CameraRay& ray) {
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

// The placeholder integrator's values of the pixel at `column` and `row`:
// kPixelValues of them, into `values`.
__device__ void RenderPixel(std::int32_t column, std::int32_t row,
                            float* values) {
  const auto* const cache = reinterpret_cast<const void*>(params.sampler_cache);
  Vector3 color;
  Vector3 normal;
  float depth = 0.0F;
  for (std::uint32_t index = 0; index < params.samples_per_pixel; ++index) {
    const device::Sampler sampler(column, row, 0, static_cast<int>(index),
                                  cache);
    const device::CameraRay ray = device::GenerateRay(
        params.camera, column, row, device::DrawCameraSamples(sampler));
    color = color +
            ((0.5F * ray.direction) + Vector3{.x = 0.5F, .y = 0.5F, .z = 0.5F});
    const Trace hit = TraceRay(ray);
    if (hit.distance != device::kNoHit) {
      depth += hit.distance;
      normal = normal + hit.normal;
    } else {
      depth = device::kNoHit;
    }
  }
  const float scale = 1.0F / static_cast<float>(params.samples_per_pixel);
  const float pixel[kPixelValues] = {
      scale * color.x, scale * color.y,  scale * color.z,  1.0F,
      scale * depth,   scale * normal.x, scale * normal.y, scale * normal.z,
  };
  for (std::uint32_t value = 0; value < kPixelValues; ++value) {
    values[value] = pixel[value];
  }
}

// Writes `values`, the pixel at `pixel` of the data window, into the film's
// planes.
__device__ void WritePixel(const float* values, std::uint64_t pixel) {
  const auto* const planes = reinterpret_cast<const FilmPlane*>(params.planes);
  auto* const film = reinterpret_cast<float*>(params.film);
  for (std::uint32_t index = 0; index < params.plane_count; ++index) {
    const FilmPlane& plane = planes[index];
    float* const out = film + plane.offset + (pixel * plane.channels);
    for (std::uint32_t channel = 0; channel < plane.channels; ++channel) {
      out[channel] =
          channel < plane.values ? values[plane.first_value + channel] : 0.0F;
    }
  }
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
  using umbra::device::gpu::backend::kPixelValues;
  const uint3 index = optixGetLaunchIndex();
  const auto* const tiles =
      reinterpret_cast<const umbra::device::PixelWindow*>(params.tiles);
  const umbra::device::PixelWindow& tile = tiles[index.z];
  const std::int32_t column = tile.x_min + static_cast<std::int32_t>(index.x);
  const std::int32_t row = tile.y_min + static_cast<std::int32_t>(index.y);
  if (column >= tile.x_end || row >= tile.y_end) {
    return;
  }
  float values[kPixelValues];
  umbra::device::gpu::backend::RenderPixel(column, row, values);
  const auto width =
      static_cast<std::uint64_t>(params.window.x_end - params.window.x_min);
  const std::uint64_t pixel =
      (static_cast<std::uint64_t>(row - params.window.y_min) * width) +
      static_cast<std::uint64_t>(column - params.window.x_min);
  umbra::device::gpu::backend::WritePixel(values, pixel);
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
