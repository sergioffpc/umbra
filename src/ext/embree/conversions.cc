// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/embree/conversions.h"

#include <embree4/rtcore_common.h>
#include <embree4/rtcore_geometry.h>
#include <embree4/rtcore_ray.h>

#include <array>
#include <cstddef>
#include <span>

#include "umbra/core/safety.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/render_data.h"

namespace umbra::ext::embree {

std::array<float, 16> EmbreeMatrix(const device::Matrix& matrix) {
  std::array<float, 16> floats{};
  for (std::size_t index = 0; index < floats.size(); ++index) {
    floats.at(index) = static_cast<float>(matrix.at(index));
  }
  return floats;
}

RTCBounds EmbreeBounds(const device::ShapeBounds& bounds) {
  return {
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

std::span<const int> Lanes(const int* valid, unsigned int count)
    [[clang::nonblocking]] {
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

}  // namespace umbra::ext::embree
