// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// How a CPU device traces a ray: it converts it to Embree's, intersects it
// with its top level, and turns Embree's hit into a Hit, on the hot path
// (docs/adr/0003-no-allocations-on-the-hot-path.md).

#ifndef UMBRA_DEVICE_CPU_TRACE_H_
#define UMBRA_DEVICE_CPU_TRACE_H_

#include <embree4/rtcore_device.h>

#include <optional>
#include <span>

#include "bvh.h"
#include "umbra/core/safety.h"
#include "umbra/device/cpu/cpu_device.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device::cpu::internal {

// What `ray` meets first in `top`, whose instances place `placements`, by
// instance id; none when it meets nothing, or when there is no top level.
[[nodiscard]] std::optional<Hit> Intersect(
    RTCScene UMBRA_NULLABLE top, std::span<const Placement> placements,
    const Ray& ray) [[clang::nonblocking]];

}  // namespace umbra::device::cpu::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_CPU_TRACE_H_
