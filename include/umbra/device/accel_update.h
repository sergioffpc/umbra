// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// What every device does to a prototype's acceleration structure when an
// update changes the prototype: keep it, refit it or rebuild it, and why
// (docs/adr/0013-devices-render-from-their-own-memory.md). Every device
// decides with it, and builds, refits and logs in its own way.

#ifndef UMBRA_DEVICE_ACCEL_UPDATE_H_
#define UMBRA_DEVICE_ACCEL_UPDATE_H_

#include <cstddef>
#include <cstdint>

#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device {

// The refits in a row after which a device rebuilds, instead of refitting
// again, the acceleration structure of a prototype whose points change:
// every refit loosens its bounds.
inline constexpr int kMaxRefits = 8;

// How a prototype's acceleration structure was built.
enum class AccelKind : std::uint8_t {
  // For tracing alone, as static geometry is.
  kStatic,
  // To be refitted when its points change, at some cost in tracing.
  kRefittable,
};

// The points an acceleration structure is built or refitted from: the
// motion samples of a prototype's points, and the points of each.
struct AccelPoints {
  std::size_t motion_samples = 0;
  std::size_t count = 0;

  friend bool operator==(const AccelPoints&, const AccelPoints&) = default;
};

// The points of `prototype`.
[[nodiscard]] AccelPoints AccelPointsOf(const Prototype& prototype);

// A prototype's acceleration structure, as a device last built or refitted
// it.
struct HeldAccel {
  AccelKind kind = AccelKind::kStatic;
  // The points it was built or last refitted from.
  AccelPoints points;
  // The refits in a row since it was built.
  int refits = 0;
};

// What a device does to a prototype's acceleration structure at an update,
// and why it rebuilds it rather than refits it.
enum class AccelUpdate : std::uint8_t {
  // Keeps it: neither the topology nor the points changed.
  kKeep,
  // Refits it to the new points.
  kRefit,
  // Rebuilds it: the topology changed.
  kRebuildTopology,
  // Rebuilds it to be refitted: the points changed, and it was built for
  // tracing alone.
  kRebuildToRefit,
  // Rebuilds it: the number of motion samples, or of points, changed, which
  // a refit cannot follow.
  kRebuildPoints,
  // Rebuilds it: it was refitted kMaxRefits times in a row.
  kRebuildAfterRefits,
};

// What a device does to `held` at an update with `changes`, after which
// the prototype has `points`.
[[nodiscard]] AccelUpdate PlanAccelUpdate(const HeldAccel& held,
                                          ChangeFlags changes,
                                          const AccelPoints& points);

}  // namespace umbra::device

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_ACCEL_UPDATE_H_
