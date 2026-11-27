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
#include <array>
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
#include <variant>
#include <vector>

#include "bvh.h"
#include "copies.h"
#include "nonblocking.h"
#include "process_device.h"
#include "umbra/core/error.h"
#include "umbra/core/floating_point.h"
#include "umbra/core/log.h"
#include "umbra/core/memory_policy.h"
#include "umbra/core/number_format.h"
#include "umbra/core/safety.h"
#include "umbra/device/camera_rays.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/device/geometry.h"
#include "umbra/device/image.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/render_data.h"
#include "umbra/device/sampler.h"

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
  RTCDevice UMBRA_NULLABLE embree = internal::ProcessRtcDevice();
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

// Where a render var's plane lies in the film.
struct FilmPlane {
  device::VarSource source = device::VarSource::kColor;
  std::size_t offset = 0;
  std::uint32_t channels = 1;
};

// The film's planes for the vars of `frame`, and the floats they need.
std::pair<std::vector<FilmPlane>, std::size_t> PlanFilm(
    const device::FrameParameters& frame) {
  const device::PixelWindow& window = frame.image.window;
  const auto pixels = static_cast<std::size_t>(window.width()) *
                      static_cast<std::size_t>(window.height());
  std::vector<FilmPlane> planes;
  std::size_t floats = 0;
  for (const device::FilmVar& var : frame.vars) {
    planes.push_back(
        {.source = var.source, .offset = floats, .channels = var.channels});
    floats += pixels * var.channels;
  }
  return {std::move(planes), floats};
}

// `vector` as Embree's rays hold it.
device::Float3 Float3Of(device::Vector3 vector) [[clang::nonblocking]] {
  return {.x = vector.x, .y = vector.y, .z = vector.z};
}

// The mean of a pixel's samples, as the placeholder integrator renders them.
struct PixelValue {
  std::array<float, 4> color{};
  float depth = 0.0F;
  std::array<float, 3> normal{};
};

// What the render of a product reads.
struct RenderInputs {
  const CpuDevice* device;
  const device::CameraModel* camera;
  std::span<const std::byte> sampler_cache;
  std::uint32_t samples_per_pixel = 1;
  // How many tiles it takes at a time.
  std::uint64_t batch_size = 1;
};

// The placeholder integrator's pixel at `column` and `row`.
PixelValue RenderPixel(const RenderInputs& inputs, std::int32_t column,
                       std::int32_t row) [[clang::nonblocking]] {
  device::Vector3 color;
  device::Vector3 normal;
  float depth = 0.0F;
  for (std::uint32_t index = 0; index < inputs.samples_per_pixel; ++index) {
    const device::Sampler sampler(column, row, 0, static_cast<int>(index),
                                  inputs.sampler_cache.data());
    const device::CameraRay camera_ray = device::GenerateRay(
        *inputs.camera, column, row, device::DrawCameraSamples(sampler));
    const device::Vector3 direction = camera_ray.direction;
    color = color + ((0.5F * direction) +
                     device::Vector3{.x = 0.5F, .y = 0.5F, .z = 0.5F});
    const std::optional<Hit> hit = inputs.device->Intersect({
        .origin = Float3Of(camera_ray.origin),
        .direction = Float3Of(direction),
        .time = camera_ray.time,
        .t_min = camera_ray.t_min,
        .t_max = camera_ray.t_max,
    });
    if (hit.has_value()) {
      depth += hit->distance;
      normal = normal + hit->normal;
    } else {
      depth = __builtin_huge_valf();
    }
  }
  const float scale = 1.0F / static_cast<float>(inputs.samples_per_pixel);
  return {
      .color = {scale * color.x, scale * color.y, scale * color.z, 1.0F},
      .depth = scale * depth,
      .normal = {scale * normal.x, scale * normal.y, scale * normal.z},
  };
}

// The channels of a var that the placeholder integrator leaves at 0.
constexpr std::array<float, 3> kZeroChannels{};

// The channels a pixel's value gives a var of `source`.
std::span<const float> ChannelsOf(const PixelValue& value,
                                  device::VarSource source)
    [[clang::nonblocking]] {
  switch (source) {
    case device::VarSource::kColor:
      return value.color;
    case device::VarSource::kDepth:
      return {&value.depth, 1};
    case device::VarSource::kNormal:
      return value.normal;
    case device::VarSource::kAlbedo:
      break;
  }
  return kZeroChannels;
}

// Writes `value`, the pixel at `pixel` of the data window, into the film's
// planes.
void WritePixel(const PixelValue& value, std::size_t pixel,
                std::span<const FilmPlane> planes, std::span<float> film)
    [[clang::nonblocking]] {
  for (const FilmPlane& plane : planes) {
    const std::span<float> out =
        film.subspan(plane.offset + (pixel * plane.channels), plane.channels);
    const std::span<const float> given = ChannelsOf(value, plane.source);
    const std::size_t copied = std::min(out.size(), given.size());
    std::ranges::copy(given.first(copied), out.begin());
    std::ranges::fill(out.subspan(copied), 0.0F);
  }
}

// Renders `tile`'s pixels into the film's planes.
void RenderTile(const RenderInputs& inputs, const device::PixelWindow& window,
                const device::PixelWindow& tile,
                std::span<const FilmPlane> planes, std::span<float> film)
    [[clang::nonblocking]] {
  const auto width = static_cast<std::size_t>(window.width());
  for (std::int32_t row = tile.y_min; row < tile.y_end; ++row) {
    for (std::int32_t column = tile.x_min; column < tile.x_end; ++column) {
      const std::size_t pixel =
          (static_cast<std::size_t>(row - window.y_min) * width) +
          static_cast<std::size_t>(column - window.x_min);
      WritePixel(RenderPixel(inputs, column, row), pixel, planes, film);
    }
  }
}

// Copies `tile`'s pixels from the film's planes into the render buffers of
// `frame`'s vars.
void CopyTile(const device::FrameParameters& frame,
              const device::PixelWindow& tile,
              std::span<const FilmPlane> planes, std::span<const float> film) {
  const device::PixelWindow& window = frame.image.window;
  const auto width = static_cast<std::size_t>(window.width());
  const auto tile_width = static_cast<std::size_t>(tile.width());
  for (const auto& [var, plane] : std::views::zip(frame.vars, planes)) {
    for (std::int32_t row = tile.y_min; row < tile.y_end; ++row) {
      const std::size_t first =
          ((static_cast<std::size_t>(row - window.y_min) * width) +
           static_cast<std::size_t>(tile.x_min - window.x_min)) *
          plane.channels;
      const std::size_t count = tile_width * plane.channels;
      std::ranges::copy(film.subspan(plane.offset + first, count),
                        var.pixels.subspan(first, count).begin());
    }
  }
}

// What a device renders a product with: its inputs, the film's planes and
// the film.
struct FilmTarget {
  std::span<const FilmPlane> planes;
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
  const auto render = [&](std::uint64_t tile) {
    const device::PixelWindow window = tiles.TileWindow(tile);
    RenderTile(inputs, frame.image.window, window, target.planes, target.film);
    CopyTile(frame, window, target.planes, target.film);
    tiles.Finish(tile);
  };
  for (device::TileQueue::Batch batch = tiles.Take(inputs.batch_size);
       !batch.IsEmpty(); batch = tiles.Take(inputs.batch_size)) {
    tbb::parallel_for(batch.first, batch.end, render);
  }
}

// Readies `state` to render `frame`, on its arena: its sampler cache, the
// first time, and a film large enough. Gives the film's planes.
std::vector<FilmPlane> Prepare(State& state,
                               const device::FrameParameters& frame) {
  if (state.sampler_cache.empty()) {
    state.sampler_cache.resize(device::kSamplerCacheSize);
    device::InitializeSamplerCache(state.sampler_cache);
    LogNode(state.name, "sampler cache", state.sampler_cache.data());
  }
  auto [planes, floats] = PlanFilm(frame);
  if (state.film.size() < floats) {
    state.film = std::vector<float>(floats);
    LogNode(state.name, "film", state.film.data());
  }
  return std::move(planes);
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
  const device::CameraModel camera =
      device::MakeCameraModel(state.cameras.at(camera_id)->view, frame);
  state.arena.execute([&] {
    const std::vector<FilmPlane> planes = Prepare(state, frame);
    const RenderInputs inputs{
        .device = this,
        .camera = &camera,
        .sampler_cache = state.sampler_cache,
        .samples_per_pixel = std::max(frame.samples_per_pixel, 1U),
        .batch_size = static_cast<std::uint64_t>(state.arena.max_concurrency()),
    };
    RenderBatches(inputs, frame, tiles, {.planes = planes, .film = state.film});
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

}  // namespace umbra::ext::embree
