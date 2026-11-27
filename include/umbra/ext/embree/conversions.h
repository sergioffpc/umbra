// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Conversions between Embree's types and umbra's: matrices, the bounds and
// rays of a user geometry's callbacks, and the hits they report.

#ifndef UMBRA_EXT_EMBREE_CONVERSIONS_H_
#define UMBRA_EXT_EMBREE_CONVERSIONS_H_

#include <embree4/rtcore_common.h>
#include <embree4/rtcore_geometry.h>
#include <embree4/rtcore_ray.h>

#include <array>
#include <span>

#include "umbra/core/safety.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::embree {

// `matrix`, row-major for row vectors, as the floats of Embree's
// column-major matrix for column vectors: the same numbers in order.
[[nodiscard]] std::array<float, 16> EmbreeMatrix(const device::Matrix& matrix);

// `bounds` as Embree's.
[[nodiscard]] RTCBounds EmbreeBounds(const device::ShapeBounds& bounds);

// The lanes of a packet of `count` rays, nonzero where a ray is valid, from
// the mask Embree passes a user geometry's callbacks.
[[nodiscard]] std::span<const int> Lanes(const int* valid UMBRA_LIFETIMEBOUND,
                                         unsigned int count)
    [[clang::nonblocking]];

// The ray Embree passes a user geometry's callbacks, in the geometry's own
// space.
[[nodiscard]] device::ShapeRay ShapeRayOf(const RTCRay& ray)
    [[clang::nonblocking]];

// The hit of `shape_hit` on the user primitive `args` intersects.
[[nodiscard]] RTCHit HitOf(const device::ShapeHit& shape_hit,
                           const RTCIntersectFunctionNArguments& args)
    [[clang::nonblocking]];

}  // namespace umbra::ext::embree

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_EMBREE_CONVERSIONS_H_
