// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "commit_log.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bvh.h"
#include "copies.h"
#include "held.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/device/accel_update.h"
#include "umbra/device/change_set.h"
#include "umbra/device/cpu/cpu_device.h"
#include "umbra/device/id_table.h"
#include "umbra/device/render_data.h"

namespace umbra::device::cpu::internal {
namespace {

// "built", "refitted, 3 of 8".
std::string DescribeEvent(const PrototypeRecord& record) {
  if (!record.event.has_value()) {
    return "built";
  }
  switch (*record.event) {
    case device::AccelUpdate::kKeep:
      return "kept";
    case device::AccelUpdate::kRebuildTopology:
      return "rebuilt, its topology changed";
    case device::AccelUpdate::kRebuildToRefit:
      return "rebuilt to be refitted, its points changed";
    case device::AccelUpdate::kRebuildPoints:
      return "rebuilt, its motion samples or points changed";
    case device::AccelUpdate::kRebuildAfterRefits:
      return std::format("rebuilt after {} refits", device::kMaxRefits);
    case device::AccelUpdate::kRefit:
      return std::format("refitted, {} of {}", record.refits,
                         device::kMaxRefits);
  }
  return {};
}

void LogPrototype(std::string_view name, const Held& held,
                  device::PrototypeId prim) {
  const PrototypeRecord* const record = held.prototypes.Find(prim);
  if (record == nullptr) {
    return;
  }
  const std::size_t samples = record->copy.view.points.size();
  const std::size_t triangles = record->copy.view.triangles.size();
  UMBRA_LOG_DEBUG(kDevice,
                  "{}: prototype {}: BVH {} ({} triangle{}, {} time step{})",
                  name, record->copy.view.path, DescribeEvent(*record),
                  core::FormatCount(triangles), triangles == 1 ? "" : "s",
                  samples, samples == 1 ? "" : "s");
}

void LogLight(std::string_view name, const Held& held, device::LightId prim) {
  const LightRecord* const record = held.lights.Find(prim);
  if (record != nullptr && record->shape.has_value()) {
    UMBRA_LOG_DEBUG(
        kDevice, "{}: light {}: BVH built ({})", name, record->copy.view.path,
        device::kLightShapeNames.at(record->copy.view.shape.index()));
  }
}

void LogTopLevel(std::string_view name, const Held& held) {
  const auto lights = static_cast<std::size_t>(
      std::ranges::count(held.placements, HitKind::kLight, &Placement::kind));
  const std::size_t prototypes = held.placements.size() - lights;
  UMBRA_LOG_DEBUG(kDevice,
                  "{}: top level rebuilt: {} prototype instances, {} light "
                  "instances",
                  name, core::FormatCount(prototypes),
                  core::FormatCount(lights));
}

}  // namespace

void LogDropped(std::string_view name, const Held& held,
                std::span<const device::PrototypeId> removed) {
  for (const device::PrototypeId prim : removed) {
    if (const PrototypeRecord* const record = held.prototypes.Find(prim);
        record != nullptr) {
      UMBRA_LOG_DEBUG(kDevice, "{}: prototype {}: BVH dropped", name,
                      record->copy.view.path);
    }
  }
}

void LogCommit(std::string_view name, const Held& held,
               const device::ChangeSet& changes) {
  if (!core::IsLogEnabled(core::LogComponent::kDevice,
                          core::LogLevel::kDebug)) {
    return;
  }
  for (const device::Prototype& prototype : changes.prototypes.added) {
    LogPrototype(name, held, prototype.id);
  }
  for (const device::UpdatedPrototype& updated : changes.prototypes.updated) {
    LogPrototype(name, held, updated.prototype.id);
  }
  for (const std::span<const device::Light> lights :
       {changes.lights.added, changes.lights.updated}) {
    for (const device::Light& light : lights) {
      LogLight(name, held, light.id);
    }
  }
  LogTopLevel(name, held);
}

}  // namespace umbra::device::cpu::internal
