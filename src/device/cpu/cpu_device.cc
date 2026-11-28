// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/cpu/cpu_device.h"

#include <embree4/rtcore_common.h>
#include <embree4/rtcore_device.h>
#include <embree4/rtcore_ray.h>
#include <embree4/rtcore_scene.h>
#include <oneapi/tbb/info.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_scheduler_observer.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bvh.h"
#include "copies.h"
#include "umbra/core/error.h"
#include "umbra/core/floating_point.h"
#include "umbra/core/log.h"
#include "umbra/core/memory_policy.h"
#include "umbra/core/number_format.h"
#include "umbra/core/safety.h"
#include "umbra/device/accel_update.h"
#include "umbra/device/camera_rays.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/device/film.h"
#include "umbra/device/geometry.h"
#include "umbra/device/image.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/render_data.h"
#include "umbra/device/sampler.h"
#include "umbra/ext/embree/nonblocking.h"
#include "umbra/ext/embree/owners.h"

namespace umbra::device::cpu {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;
using ::umbra::device::cpu::internal::BvhKind;
using ::umbra::device::cpu::internal::Placement;
using ::umbra::device::cpu::internal::Status;
using ::umbra::ext::embree::Scene;

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

struct PrototypeRecord {
  internal::PrototypeCopy copy;
  Scene bvh;
  BvhKind kind = BvhKind::kStatic;
  // The refits in a row since it was built.
  int refits = 0;
  // What the last commit did to its BVH: none when it built it, as it
  // added the prototype.
  std::optional<device::AccelUpdate> event;
};

struct LightRecord {
  internal::Copy<device::Light> copy;
  // Its shape, when rays meet it, which its BVH holds.
  std::optional<device::LightShape> shape;
  Scene bvh;
};

template <typename View>
using CopyTable = Table<internal::Copy<View>>;

// The memory policy of the calling thread before it entered a CPU device's
// arena, while it is in the arena.
std::optional<core::MemoryPolicy>& SavedPolicy() {
  thread_local std::optional<core::MemoryPolicy> policy;
  return policy;
}

// Configures every thread that enters a CPU device's arena, its workers and
// the external thread alike: its floating-point state, and its memory
// policy, bound to the device's NUMA node, so that every page first touched
// in the arena lies on the node. A thread leaving the arena gets its own
// policy back. A thread is in one device's arena at a time.
class ArenaObserver final : public tbb::task_scheduler_observer {
 public:
  ArenaObserver(tbb::task_arena& arena, int node, std::string_view name)
      : tbb::task_scheduler_observer(arena), node_(node), name_(name) {
    observe(true);
  }
  ArenaObserver(const ArenaObserver&) = delete;
  ArenaObserver& operator=(const ArenaObserver&) = delete;
  ArenaObserver(ArenaObserver&&) = delete;
  ArenaObserver& operator=(ArenaObserver&&) = delete;
  ~ArenaObserver() override { observe(false); }

  // NOLINTNEXTLINE(umbra-bool-parameter): oneTBB's interface.
  void on_scheduler_entry(bool /*is_worker*/) override {
    core::ConfigureThreadFloatingPoint();
    auto bound = core::BindThreadMemory(node_);
    if (bound.has_value()) {
      SavedPolicy() = *bound;
    } else if (!warned_.exchange(true, std::memory_order_relaxed)) {
      UMBRA_LOG_WARNING(kDevice,
                        "{}: memory not bound to NUMA node {} ({}); it lies "
                        "where the kernel places it",
                        name_, node_, bound.error().message());
    }
  }

  // NOLINTNEXTLINE(umbra-bool-parameter): oneTBB's interface.
  void on_scheduler_exit(bool /*is_worker*/) override {
    std::optional<core::MemoryPolicy>& saved = SavedPolicy();
    if (saved.has_value()) {
      core::RestoreThreadMemory(*saved);
      saved.reset();
    }
  }

 private:
  int node_;
  std::string_view name_;
  std::atomic<bool> warned_{false};
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
        observer(arena, node, name) {}

  std::string name;
  tbb::task_arena::constraints constraints;
  tbb::task_arena arena;
  ArenaObserver observer;
  RTCDevice UMBRA_NULLABLE embree = ext::embree::ProcessDevice();
  Table<PrototypeRecord> prototypes;
  Table<LightRecord> lights;
  CopyTable<device::Camera> cameras;
  CopyTable<device::MaterialNetwork> materials;
  CopyTable<device::Texture> textures;
  // The top level, and what each of its instances places.
  Scene top;
  std::vector<Placement> placements;
  // The sampler's cache, empty until the first render.
  std::vector<std::byte> sampler_cache;
  // The film: the planes of the render vars of the product rendering, one
  // after another, each laid out as its render buffer.
  std::vector<float> film;
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

// Builds `record`'s BVH anew, as `event` says why: none when it builds it
// for the first time.
Status Rebuild(PrototypeRecord& record, RTCDevice device,
               std::optional<device::AccelUpdate> event) {
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
  Status built = Rebuild(*record, device, std::nullopt);
  slot = std::move(record);
  return built;
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
  internal::UpdatePrototype(updated, record.copy);
  const device::AccelUpdate event = device::PlanAccelUpdate(
      held, updated.changes, device::AccelPointsOf(record.copy.view));
  switch (event) {
    case device::AccelUpdate::kKeep:
      record.event = event;
      return {};
    case device::AccelUpdate::kRefit:
      ++record.refits;
      record.event = event;
      return internal::RefitPrototype(device, record.bvh, record.copy.view);
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

// Copies `light`, whole, and builds the BVH of its shape, if it has one.
Status CommitLight(std::unique_ptr<LightRecord>& slot,
                   const device::Light& light, RTCDevice device) {
  auto record = std::make_unique<LightRecord>();
  record->copy.view = internal::CopyOf(light, record->copy.storage);
  record->shape = device::ShapeOf(light.shape);
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

RTCRayHit RayHitOf(const Ray& ray) [[clang::nonblocking]] {
  RTCRayHit ray_hit{};
  ray_hit.ray.org_x = ray.origin.x;
  ray_hit.ray.org_y = ray.origin.y;
  ray_hit.ray.org_z = ray.origin.z;
  ray_hit.ray.dir_x = ray.direction.x;
  ray_hit.ray.dir_y = ray.direction.y;
  ray_hit.ray.dir_z = ray.direction.z;
  ray_hit.ray.tnear = ray.t_min;
  ray_hit.ray.tfar = ray.t_max;
  ray_hit.ray.time = ray.time;
  ray_hit.ray.mask = ~0U;
  ray_hit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
  ray_hit.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
  return ray_hit;
}

// A transform as Embree gives it in RTC_FORMAT_FLOAT3X4_COLUMN_MAJOR: 12
// floats, the columns one after another.
struct Columns {
  device::Vector3 x_axis;
  device::Vector3 y_axis;
  device::Vector3 z_axis;
  device::Vector3 translation;
};
static_assert(sizeof(Columns) == 12 * sizeof(float));

// The geometric normal `normal`, in the object space of the instance a ray
// met at `time` in `top`, in world space, of unit length, facing
// `direction`.
device::Vector3 WorldNormal(RTCScene top, unsigned int instance, float time,
                            device::Vector3 normal, device::Vector3 direction)
    [[clang::nonblocking]] {
  // The instance's object-to-world transform, column-major for column
  // vectors: its columns are the images of the X, Y and Z axes.
  Columns columns;
  rtcGetGeometryTransformFromScene(top, instance, time,
                                   RTC_FORMAT_FLOAT3X4_COLUMN_MAJOR, &columns);
  const device::Vector3& x_axis = columns.x_axis;
  const device::Vector3& y_axis = columns.y_axis;
  const device::Vector3& z_axis = columns.z_axis;
  // Normals transform by the inverse transpose, which the cofactors give
  // up to the determinant: its sign is lost as the normal faces the ray.
  const device::Vector3 world =
      device::Normalize((normal.x * device::Cross(y_axis, z_axis)) +
                        (normal.y * device::Cross(z_axis, x_axis)) +
                        (normal.z * device::Cross(x_axis, y_axis)));
  return device::Dot(world, direction) > 0.0F ? -1.0F * world : world;
}

// `vector` as Embree's rays hold it.
device::Float3 Float3Of(device::Vector3 vector) [[clang::nonblocking]] {
  return {.x = vector.x, .y = vector.y, .z = vector.z};
}

// What the render of a product reads.
struct RenderInputs {
  const CpuDevice* device;
  device::PixelSampling sampling;
  // How many tiles it takes at a time.
  std::uint64_t batch_size;
};

// The first hit of `ray` in what `device` traces.
device::FirstHit TraceFirstHit(const CpuDevice& device,
                               const device::CameraRay& ray)
    [[clang::nonblocking]] {
  const std::optional<Hit> hit = device.Intersect({
      .origin = Float3Of(ray.origin),
      .direction = Float3Of(ray.direction),
      .time = ray.time,
      .t_min = ray.t_min,
      .t_max = ray.t_max,
  });
  if (!hit.has_value()) {
    return {};
  }
  return {.distance = hit->distance, .normal = hit->normal};
}

// Renders `tile`'s pixels into the film's planes.
void RenderTile(const RenderInputs& inputs, const device::PixelWindow& window,
                const device::PixelWindow& tile,
                const device::FilmLayout& layout, std::span<float> film)
    [[clang::nonblocking]] {
  const auto trace =
      [&inputs](const device::CameraRay& ray)
          [[clang::nonblocking]] { return TraceFirstHit(*inputs.device, ray); };
  for (std::int32_t row = tile.y_min; row < tile.y_end; ++row) {
    for (std::int32_t column = tile.x_min; column < tile.x_end; ++column) {
      device::WritePixel(
          device::PlaceholderPixel(inputs.sampling, column, row, trace),
          device::PixelIndex(window, column, row), layout, film);
    }
  }
}

// Copies `source` into `destination`, which is as long.
void CopyFloats(std::span<const float> source, std::span<float> destination)
    [[clang::nonblocking]] {
  auto into = destination.begin();
  for (const float value : source.first(destination.size())) {
    *into = value;
    ++into;
  }
}

// Copies `tile`'s pixels from the film's planes into the render buffers of
// `frame`'s vars.
void CopyTile(const device::FrameParameters& frame,
              const device::PixelWindow& tile, const device::FilmLayout& layout,
              std::span<const float> film) [[clang::nonblocking]] {
  const auto tile_width = static_cast<std::size_t>(tile.width());
  for (const auto& [var, plane] :
       std::views::zip(frame.vars, layout.Planes())) {
    const std::size_t count = tile_width * plane.channels;
    for (std::int32_t row = tile.y_min; row < tile.y_end; ++row) {
      const std::size_t first =
          device::TileRowOffset(frame.image.window, tile, row, plane.channels);
      CopyFloats(film.subspan(plane.offset + first, count),
                 var.pixels.subspan(first, count));
    }
  }
}

// What a device renders a product into: the film's layout and the film.
struct FilmTarget {
  const device::FilmLayout* layout;
  std::span<float> film;
};

// Logs, at debug level, the NUMA node of `what`, which `device` allocated
// and touched at `address`.
void LogNode(std::string_view device, std::string_view what,
             const void* address) {
  if (!core::IsLogEnabled(core::LogComponent::kDevice,
                          core::LogLevel::kDebug)) {
    return;
  }
  const auto node = core::NodeOfPage(address);
  UMBRA_LOG_DEBUG(kDevice, "{}: {} on NUMA node {}", device, what,
                  node.has_value()
                      ? std::format("{}", *node)
                      : std::format("unknown ({})", node.error().message()));
}

// Renders the tiles it takes from `tiles`, a batch at a time, on the arena
// it runs in, into `target`: it copies each tile into the render buffers of
// `frame`'s vars as soon as it is rendered, then finishes it.
void RenderBatches(const RenderInputs& inputs,
                   const device::FrameParameters& frame,
                   device::TileQueue& tiles, const FilmTarget& target) {
  const auto render = [&](std::uint64_t tile) [[clang::nonblocking]] {
    const device::PixelWindow window = tiles.TileWindow(tile);
    RenderTile(inputs, frame.image.window, window, *target.layout, target.film);
    CopyTile(frame, window, *target.layout, target.film);
    tiles.Finish(tile);
  };
  for (device::TileQueue::Batch batch = tiles.Take(inputs.batch_size);
       !batch.IsEmpty(); batch = tiles.Take(inputs.batch_size)) {
    tbb::parallel_for(batch.first, batch.end, render);
  }
}

// Readies `state` to render a film of `layout`, on its arena: its sampler
// cache, the first time, and a film large enough.
void Prepare(State& state, const device::FilmLayout& layout) {
  if (state.sampler_cache.empty()) {
    state.sampler_cache.resize(device::kSamplerCacheSize);
    device::InitializeSamplerCache(state.sampler_cache);
    LogNode(state.name, "sampler cache", state.sampler_cache.data());
  }
  if (state.film.size() < layout.floats) {
    state.film = std::vector<float>(layout.floats);
    LogNode(state.name, "film", state.film.data());
  }
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

std::expected<void, Error> CpuDevice::Render(
    const device::FrameParameters& frame, device::TileQueue& tiles) {
  State& state = *state_;
  const auto camera_id = std::to_underlying(frame.camera);
  if (camera_id >= state.cameras.size() ||
      state.cameras.at(camera_id) == nullptr) {
    return std::unexpected(
        Error(ErrorCode::kInternal,
              std::format("{} holds no camera {}", state.name, camera_id)));
  }
  const std::optional<device::FilmLayout> layout = device::PlanFilm(frame);
  if (!layout.has_value()) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("{} renders at most {} vars, not {}", state.name,
                          device::kMaxFilmPlanes, frame.vars.size())));
  }
  const device::CameraModel camera =
      device::MakeCameraModel(state.cameras.at(camera_id)->view, frame);
  state.arena.execute([&] {
    Prepare(state, *layout);
    const RenderInputs inputs{
        .device = this,
        .sampling =
            {
                .camera = &camera,
                .sampler_cache = state.sampler_cache.data(),
                .samples_per_pixel = frame.samples_per_pixel,
            },
        .batch_size = static_cast<std::uint64_t>(state.arena.max_concurrency()),
    };
    RenderBatches(inputs, frame, tiles,
                  {.layout = &*layout, .film = state.film});
  });
  return {};
}

std::optional<Hit> CpuDevice::Intersect(const Ray& ray) const
    [[clang::nonblocking]] {
  RTCScene UMBRA_NULLABLE const top = state_->top.get();
  if (top == nullptr) {
    return std::nullopt;
  }
  RTCRayHit ray_hit = RayHitOf(ray);
  rtcIntersect1(top, &ray_hit, nullptr);
  if (ray_hit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
    return std::nullopt;
  }
  // at() may throw: the hardened span checks the bounds and aborts.
  const Placement& placement =
      std::span(state_->placements).subspan(ray_hit.hit.instID[0], 1).front();
  return Hit{
      .kind = placement.kind,
      .id = placement.id,
      .instance = placement.instance,
      .primitive =
          placement.kind == HitKind::kPrototype ? ray_hit.hit.primID : 0U,
      .distance = ray_hit.ray.tfar,
      .normal = WorldNormal(
          top, ray_hit.hit.instID[0], ray.time,
          {.x = ray_hit.hit.Ng_x, .y = ray_hit.hit.Ng_y, .z = ray_hit.hit.Ng_z},
          {.x = ray.direction.x, .y = ray.direction.y, .z = ray.direction.z}),
  };
}

}  // namespace umbra::device::cpu
