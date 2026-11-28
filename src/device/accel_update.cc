// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/accel_update.h"

#include "umbra/device/change_set.h"
#include "umbra/device/render_data.h"

namespace umbra::device {

AccelPoints AccelPointsOf(const Prototype& prototype) {
  return {
      .motion_samples = prototype.points.size(),
      .count = prototype.points.empty() ? 0 : prototype.points.front().size(),
  };
}

AccelUpdate PlanAccelUpdate(const HeldAccel& held, ChangeFlags changes,
                            const AccelPoints& points) {
  if (HasAny(changes, ChangeFlags::kTopology)) {
    return AccelUpdate::kRebuildTopology;
  }
  if (!HasAny(changes, ChangeFlags::kPoints)) {
    return AccelUpdate::kKeep;
  }
  if (held.kind == AccelKind::kStatic) {
    return AccelUpdate::kRebuildToRefit;
  }
  if (held.points != points) {
    return AccelUpdate::kRebuildPoints;
  }
  if (held.refits >= kMaxRefits) {
    return AccelUpdate::kRebuildAfterRefits;
  }
  return AccelUpdate::kRefit;
}

}  // namespace umbra::device
