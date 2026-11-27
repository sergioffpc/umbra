// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// A kernel that calls the single-source code, for nvcc to compile it: camera
// rays, the pixel filter, the sampler, the light shapes and the film.
// Nothing runs it.

#include <cstdint>

#include "umbra/device/camera_rays.h"
#include "umbra/device/film.h"
#include "umbra/device/geometry.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/pixel_filter.h"
#include "umbra/device/sampler.h"

namespace umbra::device {

__global__ void CallSingleSource(const CameraModel* model, const void* cache,
                                 const LightShape* shape,
                                 const FilmLayout* layout, float* out) {
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
  const PixelSampling sampling{
      .camera = model, .sampler_cache = cache, .samples_per_pixel = 1};
  const auto trace = [&](const CameraRay&) {
    return FirstHit{.distance = hit.distance, .normal = hit.normal};
  };
  const PixelWindow window{.x_min = 0, .y_min = 0, .x_end = 32, .y_end = 1};
  WritePixel(PlaceholderPixel(sampling, column, 0, trace),
             PixelIndex(window, column, 0), *layout,
             {out, TileRowOffset(window, window, 1, 1)});
}

}  // namespace umbra::device
