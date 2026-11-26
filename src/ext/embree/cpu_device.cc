// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/embree/cpu_device.h"

#include <embree4/rtcore_common.h>
#include <embree4/rtcore_device.h>
#include <embree4/rtcore_ray.h>
#include <embree4/rtcore_scene.h>
#include <oneapi/tbb/info.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_scheduler_observer.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "bvh.h"
#include "copies.h"
#include "process_device.h"
#include "umbra/core/error.h"
#include "umbra/core/floating_point.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/render_data.h"

namespace umbra::ext::embree {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;
using ::umbra::device::ChangeFlags;
using ::umbra::device::HasAny;
using ::umbra::ext::embree::internal::BvhKind;
using ::umbra::ext::embree::internal::Placement;
using ::umbra::ext::embree::internal::Scene;
using ::umbra::ext::embree::internal::Status;

// Records by id: null where the device holds no prim.
template <typename Record>
using Table = std::vector<std::unique_ptr<Record>>;

template <typename Record, typename Id>
std::unique_ptr<Record>& Slot(Table<Record>& table, Id prim) {
  return table.at(std::to_underlying(prim));
}

// The element of `values` at `index`, checked.
template <typename T>
const T& ElementAt(std::span<const T> values, std::size_t index) {
  return values.subspan(index, 1).front();
}

// What a commit did to a prototype's BVH.
enum class BvhEvent : std::uint8_t {
  kKept,
  kBuilt,
  kRebuiltTopology,
  kRebuiltToRefit,
  kRebuiltSamples,
  kRebuiltAfterRefits,
  kRefitted,
};

struct PrototypeRecord {
  internal::PrototypeCopy copy;
  Scene bvh;
  BvhKind kind = BvhKind::kStatic;
  // The refits in a row since it was built.
  int refits = 0;
  BvhEvent event = BvhEvent::kKept;
};

struct LightRecord {
  internal::Copy<device::Light> copy;
  // Its shape, when rays meet it, which its BVH holds.
  std::optional<device::LightShape> shape;
  Scene bvh;
};

template <typename View>
using CopyTable = Table<internal::Copy<View>>;

// Configures the floating-point state of every thread that enters an arena.
class FloatingPointObserver final : public tbb::task_scheduler_observer {
 public:
  explicit FloatingPointObserver(tbb::task_arena& arena)
      : tbb::task_scheduler_observer(arena) {
    observe(true);
  }
  FloatingPointObserver(const FloatingPointObserver&) = delete;
  FloatingPointObserver& operator=(const FloatingPointObserver&) = delete;
  FloatingPointObserver(FloatingPointObserver&&) = delete;
  FloatingPointObserver& operator=(FloatingPointObserver&&) = delete;
  ~FloatingPointObserver() override { observe(false); }

  // NOLINTNEXTLINE(umbra-bool-parameter): oneTBB's interface.
  void on_scheduler_entry(bool /*is_worker*/) override {
    core::ConfigureThreadFloatingPoint();
  }
};

// The constraints of the arena of NUMA node `node`: bound to it when
// oneTBB knows of it.
tbb::task_arena::constraints ConstraintsOf(int node) {
  const std::vector<tbb::numa_node_id> nodes = tbb::info::numa_nodes();
  if (std::ranges::contains(nodes, node)) {
    return tbb::task_arena::constraints(node);
  }
  return {};
}

}  // namespace

struct CpuDevice::State {
  explicit State(int node)
      : name(std::format("cpu{}", node)),
        constraints(ConstraintsOf(node)),
        arena(constraints),
        observer(arena) {}

  std::string name;
  tbb::task_arena::constraints constraints;
  tbb::task_arena arena;
  FloatingPointObserver observer;
  RTCDevice UMBRA_NULLABLE embree = internal::ProcessRtcDevice();
  Table<PrototypeRecord> prototypes;
  Table<LightRecord> lights;
  CopyTable<device::Camera> cameras;
  CopyTable<device::MaterialNetwork> materials;
  CopyTable<device::Texture> textures;
  // The top level, and what each of its instances places.
  Scene top;
  std::vector<Placement> placements;
};

namespace {

using State = CpuDevice::State;

template <typename Record>
void Grow(Table<Record>& table, std::uint32_t count) {
  if (table.size() < count) {
    table.resize(count);
  }
}

void GrowTables(State& state, const device::IdCounts& ids) {
  Grow(state.prototypes, ids.prototypes);
  Grow(state.lights, ids.lights);
  Grow(state.cameras, ids.cameras);
  Grow(state.materials, ids.materials);
  Grow(state.textures, ids.textures);
}

template <typename Record, typename Id>
void Drop(Table<Record>& table, std::span<const Id> removed) {
  for (const Id prim : removed) {
    Slot(table, prim).reset();
  }
}

void DropRemoved(State& state, const device::ChangeSet& changes) {
  for (const device::PrototypeId prim : changes.prototypes.removed) {
    if (const auto& record = Slot(state.prototypes, prim); record != nullptr) {
      UMBRA_LOG_DEBUG(kDevice, "{}: prototype {}: BVH dropped", state.name,
                      record->copy.view.path);
    }
  }
  Drop(state.prototypes, changes.prototypes.removed);
  Drop(state.lights, changes.lights.removed);
  Drop(state.cameras, changes.cameras.removed);
  Drop(state.materials, changes.materials.removed);
  Drop(state.textures, changes.textures.removed);
}

// Builds `record`'s BVH anew, as `event` says why.
Status Rebuild(PrototypeRecord& record, RTCDevice device, BvhEvent event) {
  auto bvh = internal::BuildPrototype(device, record.copy.view, record.kind);
  if (!bvh.has_value()) {
    return std::unexpected(std::move(bvh.error()));
  }
  record.bvh = *std::move(bvh);
  record.refits = 0;
  record.event = event;
  return {};
}

Status AddPrototype(std::unique_ptr<PrototypeRecord>& slot,
                    const device::Prototype& prototype, RTCDevice device) {
  auto record = std::make_unique<PrototypeRecord>();
  internal::CopyPrototype(prototype, record->copy);
  Status built = Rebuild(*record, device, BvhEvent::kBuilt);
  slot = std::move(record);
  return built;
}

// What an update with `changes` does to `record`'s BVH, whose points had
// `samples` motion samples before it.
BvhEvent Plan(const PrototypeRecord& record, ChangeFlags changes,
              std::size_t samples) {
  if (HasAny(changes, ChangeFlags::kTopology)) {
    return BvhEvent::kRebuiltTopology;
  }
  if (!HasAny(changes, ChangeFlags::kPoints)) {
    return BvhEvent::kKept;
  }
  if (record.kind == BvhKind::kStatic) {
    return BvhEvent::kRebuiltToRefit;
  }
  if (record.copy.view.points.size() != samples) {
    return BvhEvent::kRebuiltSamples;
  }
  if (record.refits >= device::kMaxRefits) {
    return BvhEvent::kRebuiltAfterRefits;
  }
  return BvhEvent::kRefitted;
}

Status ApplyUpdate(PrototypeRecord& record,
                   const device::UpdatedPrototype& updated, RTCDevice device) {
  const std::size_t samples = record.copy.view.points.size();
  internal::UpdatePrototype(updated, record.copy);
  const BvhEvent event = Plan(record, updated.changes, samples);
  switch (event) {
    case BvhEvent::kKept:
      record.event = event;
      return {};
    case BvhEvent::kRefitted:
      ++record.refits;
      record.event = event;
      return internal::RefitPrototype(device, record.bvh, record.copy.view);
    case BvhEvent::kRebuiltToRefit:
      record.kind = BvhKind::kRefittable;
      return Rebuild(record, device, event);
    case BvhEvent::kBuilt:
    case BvhEvent::kRebuiltTopology:
    case BvhEvent::kRebuiltSamples:
    case BvhEvent::kRebuiltAfterRefits:
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

// Copies the prototypes added and updated, and builds, refits or rebuilds
// their BVHs, in parallel on the arena.
Status CommitPrototypes(State& state, const device::PrototypeChanges& changes,
                        RTCDevice device) {
  std::vector<Status> added(changes.added.size());
  tbb::parallel_for(
      std::size_t{0}, changes.added.size(), [&](std::size_t index) {
        const device::Prototype& prototype = ElementAt(changes.added, index);
        added.at(index) = AddPrototype(Slot(state.prototypes, prototype.id),
                                       prototype, device);
      });
  std::vector<Status> updated(changes.updated.size());
  tbb::parallel_for(
      std::size_t{0}, changes.updated.size(), [&](std::size_t index) {
        const device::UpdatedPrototype& update =
            ElementAt(changes.updated, index);
        PrototypeRecord* const UMBRA_NULLABLE record =
            Slot(state.prototypes, update.prototype.id).get();
        updated.at(index) =
            record == nullptr
                ? Status(std::unexpected(
                      Error(ErrorCode::kInternal,
                            std::format("prototype {} is updated, but not held",
                                        update.prototype.path))))
                : ApplyUpdate(*record, update, device);
      });
  return FirstError(added).and_then([&] { return FirstError(updated); });
}

// The shape of a light that rays meet, or none for a distant or a dome
// light.
std::optional<device::LightShape> ShapeOf(const device::Light::Shape& shape) {
  using device::LightShapeKind;
  if (const auto* sphere = std::get_if<device::Light::Sphere>(&shape)) {
    return device::LightShape{
        .kind = LightShapeKind::kSphere,
        .radius = static_cast<float>(sphere->radius),
    };
  }
  if (const auto* rect = std::get_if<device::Light::Rect>(&shape)) {
    return device::LightShape{
        .kind = LightShapeKind::kRect,
        .width = static_cast<float>(rect->width),
        .height = static_cast<float>(rect->height),
    };
  }
  if (const auto* disk = std::get_if<device::Light::Disk>(&shape)) {
    return device::LightShape{
        .kind = LightShapeKind::kDisk,
        .radius = static_cast<float>(disk->radius),
    };
  }
  if (const auto* cylinder = std::get_if<device::Light::Cylinder>(&shape)) {
    return device::LightShape{
        .kind = LightShapeKind::kCylinder,
        .radius = static_cast<float>(cylinder->radius),
        .length = static_cast<float>(cylinder->length),
    };
  }
  return std::nullopt;
}

// Copies `light`, whole, and builds the BVH of its shape, if it has one.
Status CommitLight(std::unique_ptr<LightRecord>& slot,
                   const device::Light& light, RTCDevice device) {
  auto record = std::make_unique<LightRecord>();
  record->copy.view = internal::CopyOf(light, record->copy.storage);
  record->shape = ShapeOf(light.shape);
  if (record->shape.has_value()) {
    auto bvh = internal::BuildLight(device, *record->shape);
    if (!bvh.has_value()) {
      return std::unexpected(std::move(bvh.error()));
    }
    record->bvh = *std::move(bvh);
  }
  slot = std::move(record);
  return {};
}

Status CommitLights(State& state, std::span<const device::Light> lights,
                    RTCDevice device) {
  for (const device::Light& light : lights) {
    if (auto done = CommitLight(Slot(state.lights, light.id), light, device);
        !done.has_value()) {
      return done;
    }
  }
  return {};
}

// Copies the prims of one kind added and updated, whole.
template <typename View, typename Id>
void CommitCopies(CopyTable<View>& table,
                  const device::Changes<View, Id>& changes) {
  for (const std::span<const View> views : {changes.added, changes.updated}) {
    for (const View& view : views) {
      auto copy = std::make_unique<internal::Copy<View>>();
      copy->view = internal::CopyOf(view, copy->storage);
      Slot(table, view.id) = std::move(copy);
    }
  }
}

// Places every instance of `record`, of the prim `prim` of `kind`, in
// `top`.
template <typename Record>
void PlaceInstances(const Record& record, HitKind kind, std::uint32_t prim,
                    internal::TopLevel& top) {
  std::uint32_t instance = 0;
  for (const device::Instance& placed : record.copy.view.instances) {
    top.Add(record.bvh, placed,
            {.kind = kind, .id = prim, .instance = instance++});
  }
}

// Places every instance of the records of `table` in `top`.
template <typename Record>
void PlaceAll(const Table<Record>& table, HitKind kind,
              internal::TopLevel& top) {
  std::uint32_t prim = 0;
  for (const std::unique_ptr<Record>& record : table) {
    if (record != nullptr) {
      PlaceInstances(*record, kind, prim, top);
    }
    ++prim;
  }
}

Status RebuildTopLevel(State& state, RTCDevice device) {
  internal::TopLevel top(device);
  PlaceAll(state.prototypes, HitKind::kPrototype, top);
  PlaceAll(state.lights, HitKind::kLight, top);
  auto built = std::move(top).Build();
  if (!built.has_value()) {
    return std::unexpected(std::move(built.error()));
  }
  state.top = std::move(built->first);
  state.placements = std::move(built->second);
  return {};
}

// "built", "refitted, 3 of 8".
std::string DescribeEvent(const PrototypeRecord& record) {
  switch (record.event) {
    case BvhEvent::kKept:
      return "kept";
    case BvhEvent::kBuilt:
      return "built";
    case BvhEvent::kRebuiltTopology:
      return "rebuilt, its topology changed";
    case BvhEvent::kRebuiltToRefit:
      return "rebuilt to be refitted, its points changed";
    case BvhEvent::kRebuiltSamples:
      return "rebuilt, its motion samples changed";
    case BvhEvent::kRebuiltAfterRefits:
      return std::format("rebuilt after {} refits", device::kMaxRefits);
    case BvhEvent::kRefitted:
      return std::format("refitted, {} of {}", record.refits,
                         device::kMaxRefits);
  }
  return {};
}

void LogPrototype(const State& state, device::PrototypeId prim) {
  const PrototypeRecord& record =
      *state.prototypes.at(std::to_underlying(prim));
  const std::size_t samples = record.copy.view.points.size();
  const std::size_t triangles = record.copy.view.triangles.size();
  UMBRA_LOG_DEBUG(kDevice,
                  "{}: prototype {}: BVH {} ({} triangle{}, {} time step{})",
                  state.name, record.copy.view.path, DescribeEvent(record),
                  core::FormatCount(triangles), triangles == 1 ? "" : "s",
                  samples, samples == 1 ? "" : "s");
}

void LogLight(const State& state, device::LightId prim) {
  const LightRecord& record = *state.lights.at(std::to_underlying(prim));
  if (record.shape.has_value()) {
    UMBRA_LOG_DEBUG(
        kDevice, "{}: light {}: BVH built ({})", state.name,
        record.copy.view.path,
        device::kLightShapeNames.at(record.copy.view.shape.index()));
  }
}

void LogTopLevel(const State& state) {
  const auto lights = static_cast<std::size_t>(
      std::ranges::count(state.placements, HitKind::kLight, &Placement::kind));
  const std::size_t prototypes = state.placements.size() - lights;
  UMBRA_LOG_DEBUG(kDevice,
                  "{}: top level rebuilt: {} prototype instances, {} light "
                  "instances",
                  state.name, core::FormatCount(prototypes),
                  core::FormatCount(lights));
}

// Logs, as the device component at debug level, what the commit of
// `changes` built.
void LogCommit(const State& state, const device::ChangeSet& changes) {
  if (!core::IsLogEnabled(core::LogComponent::kDevice,
                          core::LogLevel::kDebug)) {
    return;
  }
  for (const device::Prototype& prototype : changes.prototypes.added) {
    LogPrototype(state, prototype.id);
  }
  for (const device::UpdatedPrototype& updated : changes.prototypes.updated) {
    LogPrototype(state, updated.prototype.id);
  }
  for (const std::span<const device::Light> lights :
       {changes.lights.added, changes.lights.updated}) {
    for (const device::Light& light : lights) {
      LogLight(state, light.id);
    }
  }
  LogTopLevel(state);
}

// Applies `changes` to what `state` holds, on the calling thread's arena.
Status Apply(State& state, const device::ChangeSet& changes, RTCDevice device) {
  GrowTables(state, changes.ids);
  DropRemoved(state, changes);
  if (auto done = CommitPrototypes(state, changes.prototypes, device);
      !done.has_value()) {
    return done;
  }
  if (auto done =
          CommitLights(state, changes.lights.added, device).and_then([&] {
            return CommitLights(state, changes.lights.updated, device);
          });
      !done.has_value()) {
    return done;
  }
  CommitCopies(state.cameras, changes.cameras);
  CommitCopies(state.materials, changes.materials);
  CommitCopies(state.textures, changes.textures);
  if (auto done = RebuildTopLevel(state, device); !done.has_value()) {
    return done;
  }
  LogCommit(state, changes);
  return {};
}

RTCRayHit RayHitOf(const Ray& ray) {
  RTCRayHit ray_hit{};
  ray_hit.ray.org_x = ray.origin.x;
  ray_hit.ray.org_y = ray.origin.y;
  ray_hit.ray.org_z = ray.origin.z;
  ray_hit.ray.dir_x = ray.direction.x;
  ray_hit.ray.dir_y = ray.direction.y;
  ray_hit.ray.dir_z = ray.direction.z;
  ray_hit.ray.tnear = 0.0F;
  ray_hit.ray.tfar = ray.t_max;
  ray_hit.ray.time = ray.time;
  ray_hit.ray.mask = ~0U;
  ray_hit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
  ray_hit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
  return ray_hit;
}

}  // namespace

CpuDevice::CpuDevice(int node) : state_(std::make_unique<State>(node)) {
  state_->arena.initialize();
  UMBRA_LOG_DEBUG(
      kDevice, "{}: arena of {} threads, {}", state_->name,
      state_->arena.max_concurrency(),
      state_->constraints.numa_id == tbb::task_arena::automatic
          ? std::string("on no NUMA node")
          : std::format("on NUMA node {}", state_->constraints.numa_id));
}

CpuDevice::~CpuDevice() = default;

std::string_view CpuDevice::name() const { return state_->name; }

std::expected<void, Error> CpuDevice::Commit(const device::ChangeSet& changes) {
  State& state = *state_;
  RTCDevice UMBRA_NULLABLE device = state.embree;
  if (device == nullptr) {
    return std::unexpected(
        Error(ErrorCode::kInternal, "Embree is not initialized"));
  }
  Status status;
  state.arena.execute([&state, &changes, device, &status] {
    status = Apply(state, changes, device);
  });
  if (!status.has_value()) {
    // The BVHs may point to copies the failed commit replaced: nothing
    // traces them until a commit succeeds.
    state.top = Scene();
    state.placements.clear();
  }
  return status;
}

std::optional<Hit> CpuDevice::Intersect(const Ray& ray) const {
  if (state_->top.get() == nullptr) {
    return std::nullopt;
  }
  RTCRayHit ray_hit = RayHitOf(ray);
  rtcIntersect1(state_->top.get(), &ray_hit);
  if (ray_hit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
    return std::nullopt;
  }
  const Placement& placement = state_->placements.at(ray_hit.hit.instID[0]);
  return Hit{
      .kind = placement.kind,
      .id = placement.id,
      .instance = placement.instance,
      .primitive =
          placement.kind == HitKind::kPrototype ? ray_hit.hit.primID : 0U,
      .distance = ray_hit.ray.tfar,
  };
}

}  // namespace umbra::ext::embree
