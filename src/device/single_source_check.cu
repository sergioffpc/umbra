// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// A kernel that calls the single-source code, for nvcc to compile it: camera
// rays, the pixel filter, the sampler and the light shapes. Nothing runs it.

#include <cstdint>

#include "umbra/device/camera_rays.h"
#include "umbra/device/geometry.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/pixel_filter.h"
#include "umbra/device/sampler.h"

namespace umbra::device {

__global__ void CallSingleSource(const CameraModel* model, const void* cache,
                                 const LightShape* shape, float* out) {
  const auto column = static_cast<std::int32_t>(threadIdx.x);
  const Sampler sampler(column, 0, 0, 0, cache);
  const CameraRay ray =
      GenerateRay(*model, column, 0, DrawCameraSamples(sampler));
  const ShapeRay local{
      .origin = ray.origin,
      .direction = ray.direction,
      .t_min = ray.t_min,
      .t_max = ray.t_max,
  };
  const ShapeHit hit = Intersect(*shape, local);
  out[threadIdx.x] = hit.distance + PixelFilterDensity(ray.time) +
                     SamplePixelFilter(ray.time, ray.time).x;
}

}  // namespace umbra::device
