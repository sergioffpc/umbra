// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "commit.h"

#include <embree4/rtcore_device.h>
#include <oneapi/tbb/parallel_for.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "bvh.h"
#include "commit_log.h"
#include "copies.h"
#include "held.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/accel_update.h"
#include "umbra/device/change_set.h"
#include "umbra/device/cpu/cpu_device.h"
#include "umbra/device/id_table.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/render_data.h"

namespace umbra::device::cpu::internal {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

// The names of the prims of each kind in messages.
constexpr std::string_view kPrototype = "prototype";
constexpr std::string_view kLight = "light";
constexpr std::string_view kCamera = "camera";
constexpr std::string_view kMaterial = "material";
constexpr std::string_view kTexture = "texture";

// The element of `values` at `index`, checked.
template <typename T>
const T& ElementAt(std::span<const T> values, std::size_t index) {
  return values.subspan(index, 1).front();
}

// Why an id does not apply, after its kind and its id.
std::string_view ReasonOf(device::IdFault fault) {
  switch (fault) {
    case device::IdFault::kOutOfRange:
      return "is not below the number of their ids";
    case device::IdFault::kRemovedNotHeld:
      return "is removed, but not held";
    case device::IdFault::kUpdatedNotHeld:
      return "is updated, but not held";
    case device::IdFault::kUpdatedTwice:
      return "is updated twice";
    case device::IdFault::kAddedHeld:
      return "is added, but already held";
    case device::IdFault::kHeldOutOfRange:
      return "is held, but not below the number of their ids";
  }
  return "does not apply";
}

// Applies the ids of `changes` to `table`, of `count` ids of the prims
// `kind`.
template <typename Changes, typename T, typename Id>
Status ApplyKind(std::string_view kind, std::uint32_t count,
                 const Changes& changes, device::IdTable<T, Id>& table) {
  return device::ApplyIds(changes, count, table)
      .transform_error([&](const device::IdError& error) {
        return Error(
            ErrorCode::kInternal,
            std::format("{} {} {}", kind, error.id, ReasonOf(error.fault)));
      });
}

// Applies the ids of `changes` to every table of `held`.
Status ApplyAllIds(Held& held, const device::ChangeSet& changes) {
  return ApplyKind(kPrototype, changes.ids.prototypes, changes.prototypes,
                   held.prototypes)
      .and_then([&] {
        return ApplyKind(kLight, changes.ids.lights, changes.lights,
                         held.lights);
      })
      .and_then([&] {
        return ApplyKind(kCamera, changes.ids.cameras, changes.cameras,
                         held.cameras);
      })
      .and_then([&] {
        return ApplyKind(kMaterial, changes.ids.materials, changes.materials,
                         held.materials);
      })
      .and_then([&] {
        return ApplyKind(kTexture, changes.ids.textures, changes.textures,
                         held.textures);
      });
}

// What `table`, of the prims `kind`, holds under `key`, which the ids
// applied hold.
template <typename T, typename Id>
std::expected<T*, Error> HeldIn(device::IdTable<T, Id>& table, Id key,
                                std::string_view kind) {
  T* UMBRA_NULLABLE const held = table.Find(key);
  if (held == nullptr) {
    return std::unexpected(
        Error(ErrorCode::kInternal,
              std::format("{} {} is not held", kind, std::to_underlying(key))));
  }
  return held;
}

// Builds `record`'s BVH anew, as `event` says why: none when it builds it
// for the first time.
Status Rebuild(PrototypeRecord& record, RTCDevice device,
               std::optional<device::AccelUpdate> event) {
  auto bvh = BuildPrototype(device, record.copy.view, record.kind);
  if (!bvh.has_value()) {
    return std::unexpected(std::move(bvh.error()));
  }
  record.bvh = *std::move(bvh);
  record.refits = 0;
  record.event = event;
  return {};
}

// Copies `prototype`, added, into `record`, and builds its BVH.
Status AddPrototype(PrototypeRecord& record, const device::Prototype& prototype,
                    RTCDevice device) {
  CopyPrototype(prototype, record.copy);
  return Rebuild(record, device, std::nullopt);
}

// `record`'s BVH, as it was built or last refitted, before an update.
device::HeldAccel HeldOf(const PrototypeRecord& record) {
  return {
      .kind = record.kind == BvhKind::kRefittable
                  ? device::AccelKind::kRefittable
                  : device::AccelKind::kStatic,
      .points = device::AccelPointsOf(record.copy.view),
      .refits = record.refits,
  };
}

Status ApplyUpdate(PrototypeRecord& record,
                   const device::UpdatedPrototype& updated, RTCDevice device) {
  const device::HeldAccel held = HeldOf(record);
  UpdatePrototype(updated, record.copy);
  const device::AccelUpdate event = device::PlanAccelUpdate(
      held, updated.changes, device::AccelPointsOf(record.copy.view));
  switch (event) {
    case device::AccelUpdate::kKeep:
      record.event = event;
      return {};
    case device::AccelUpdate::kRefit:
      ++record.refits;
      record.event = event;
      return RefitPrototype(device, record.bvh, record.copy.view);
    case device::AccelUpdate::kRebuildToRefit:
      record.kind = BvhKind::kRefittable;
      return Rebuild(record, device, event);
    case device::AccelUpdate::kRebuildTopology:
    case device::AccelUpdate::kRebuildPoints:
    case device::AccelUpdate::kRebuildAfterRefits:
      break;
  }
  return Rebuild(record, device, event);
}

// The first error of `statuses`, if any.
Status FirstError(std::span<const Status> statuses) {
  const auto failed = std::ranges::find_if(
      statuses, [](const Status& status) { return !status.has_value(); });
  return failed == statuses.end() ? Status() : *failed;
}

// Copies the prototypes added and updated into the records their ids
// applied hold, and builds, refits or rebuilds their BVHs, in parallel on
// the arena.
Status CommitPrototypes(Held& held, const device::PrototypeChanges& changes,
                        RTCDevice device) {
  std::vector<Status> added(changes.added.size());
  tbb::parallel_for(
      std::size_t{0}, changes.added.size(), [&](std::size_t index) {
        const device::Prototype& prototype = ElementAt(changes.added, index);
        added.at(index) = HeldIn(held.prototypes, prototype.id, kPrototype)
                              .and_then([&](PrototypeRecord* record) {
                                return AddPrototype(*record, prototype, device);
                              });
      });
  std::vector<Status> updated(changes.updated.size());
  tbb::parallel_for(
      std::size_t{0}, changes.updated.size(), [&](std::size_t index) {
        const device::UpdatedPrototype& update =
            ElementAt(changes.updated, index);
        updated.at(index) =
            HeldIn(held.prototypes, update.prototype.id, kPrototype)
                .and_then([&](PrototypeRecord* record) {
                  return ApplyUpdate(*record, update, device);
                });
      });
  return FirstError(added).and_then([&] { return FirstError(updated); });
}

// Copies `light` into `record`, whole, and builds the BVH of its shape, if
// it has one.
Status CommitLight(LightRecord& record, const device::Light& light,
                   RTCDevice device) {
  record = LightRecord();
  record.copy.view = CopyOf(light, record.copy.storage);
  record.shape = device::ShapeOf(light.shape);
  if (record.shape.has_value()) {
    auto bvh = BuildLight(device, *record.shape);
    if (!bvh.has_value()) {
      return std::unexpected(std::move(bvh.error()));
    }
    record.bvh = *std::move(bvh);
  }
  return {};
}

Status CommitLights(Held& held, std::span<const device::Light> lights,
                    RTCDevice device) {
  for (const device::Light& light : lights) {
    if (auto done = HeldIn(held.lights, light.id, kLight)
                        .and_then([&](LightRecord* record) {
                          return CommitLight(*record, light, device);
                        });
        !done.has_value()) {
      return done;
    }
  }
  return {};
}

// Copies `view` into what `table`, of the prims `kind`, holds under its id,
// whole.
template <typename View, typename Id>
Status CommitCopy(device::IdTable<Copy<View>, Id>& table, const View& view,
                  std::string_view kind) {
  return HeldIn(table, view.id, kind).transform([&](Copy<View>* copy) {
    *copy = Copy<View>();
    copy->view = CopyOf(view, copy->storage);
  });
}

// Copies each of `views` into what `table`, of the prims `kind`, holds
// under its id, whole.
template <typename View, typename Id>
Status CommitEachCopy(device::IdTable<Copy<View>, Id>& table,
                      std::span<const View> views, std::string_view kind) {
  for (const View& view : views) {
    if (auto done = CommitCopy(table, view, kind); !done.has_value()) {
      return done;
    }
  }
  return {};
}

// Copies the prims of one kind added and updated, whole.
template <typename View, typename Id>
Status CommitCopies(device::IdTable<Copy<View>, Id>& table,
                    const device::Changes<View, Id>& changes,
                    std::string_view kind) {
  return CommitEachCopy(table, changes.added, kind).and_then([&] {
    return CommitEachCopy(table, changes.updated, kind);
  });
}

// Places every instance of `record`, of the prim `prim` of `kind`, in
// `top`.
template <typename Record>
void PlaceInstances(const Record& record, HitKind kind, std::uint32_t prim,
                    TopLevel& top) {
  std::uint32_t instance = 0;
  for (const device::Instance& placed : record.copy.view.instances) {
    top.Add(record.bvh, placed,
            {.kind = kind, .id = prim, .instance = instance++});
  }
}

// Places every instance of the records of `table` in `top`.
template <typename Record, typename Id>
void PlaceAll(const device::IdTable<Record, Id>& table, HitKind kind,
              TopLevel& top) {
  table.ForEach([&](Id prim, const Record& record) {
    PlaceInstances(record, kind, std::to_underlying(prim), top);
  });
}

Status RebuildTopLevel(Held& held, RTCDevice device) {
  TopLevel top(device);
  PlaceAll(held.prototypes, HitKind::kPrototype, top);
  PlaceAll(held.lights, HitKind::kLight, top);
  auto built = std::move(top).Build();
  if (!built.has_value()) {
    return std::unexpected(std::move(built.error()));
  }
  held.top = std::move(built->first);
  held.placements = std::move(built->second);
  return {};
}

}  // namespace

Status Commit(Held& held, const device::ChangeSet& changes, RTCDevice device,
              std::string_view name) {
  LogDropped(name, held, changes.prototypes.removed);
  if (auto done =
          ApplyAllIds(held, changes)
              .and_then([&] {
                return CommitPrototypes(held, changes.prototypes, device);
              })
              .and_then([&] {
                return CommitLights(held, changes.lights.added, device);
              })
              .and_then([&] {
                return CommitLights(held, changes.lights.updated, device);
              })
              .and_then([&] {
                return CommitCopies(held.cameras, changes.cameras, kCamera);
              })
              .and_then([&] {
                return CommitCopies(held.materials, changes.materials,
                                    kMaterial);
              })
              .and_then([&] {
                return CommitCopies(held.textures, changes.textures, kTexture);
              })
              .and_then([&] { return RebuildTopLevel(held, device); });
      !done.has_value()) {
    return done;
  }
  LogCommit(name, held, changes);
  return {};
}

}  // namespace umbra::device::cpu::internal
