// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// A CPU device: one NUMA node, whose render data is Embree's BVHs, built in
// the node's own memory (docs/adr/0004-render-devices.md,
// docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_EXT_EMBREE_CPU_DEVICE_H_
#define UMBRA_EXT_EMBREE_CPU_DEVICE_H_

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::embree {

// A ray in world space, in meters. Its direction need not be of unit
// length: distances along it are in units of its direction.
struct Ray {
  device::Float3 origin;
  device::Float3 direction;
  // When it is cast, from 0 when the shutter opens to 1 when it closes.
  float time = 0.0F;
  // The farthest distance it meets something at.
  float t_max = __builtin_huge_valf();
};

// What a ray meets first.
enum class HitKind : std::uint8_t {
  kPrototype,
  kLight,
};

// Where a ray first meets a prototype or a shaped light.
struct Hit {
  HitKind kind = HitKind::kPrototype;
  // The id of the prototype or of the light.
  std::uint32_t id = 0;
  // Which of its instances.
  std::uint32_t instance = 0;
  // The triangle of a prototype; 0 for a light.
  std::uint32_t primitive = 0;
  // The distance along the ray.
  float distance = 0.0F;
};

// The CPU device of a NUMA node, "cpu<node>". Everything it does runs on
// a oneTBB arena bound to the node's cores, so that the memory it
// allocates is on the node, where its threads touch it first.
//
// At each commit, it copies into that memory every prim the change set
// gives (prototypes with their instances, lights, cameras, material
// networks and textures), applies the members an update's flags name to
// its own copy, and drops what it built from the prims removed. It builds
// one Embree BVH per prototype, with one time step per motion sample of
// its points, and one BVH per light with a shape (sphere, rect, disk and
// cylinder), of one user primitive that the light shape functions
// intersect. A top-level BVH, rebuilt at every commit, instances them,
// with one transform per motion sample of an instance that moves.
//
// kTopology rebuilds a prototype's BVH. kPoints refits it, but rebuilds it
// when it was not built to be refitted, as at the first change of its
// points, when the number of motion samples changes, and after kMaxRefits
// refits in a row. It logs, as the device component at debug level, what
// each commit built, refitted and rebuilt.
class CpuDevice final : public device::Device {
 public:
  // The device of NUMA node `node`. Its arena is bound to the node when
  // oneTBB knows of it, and covers every core otherwise, as on a machine
  // without NUMA.
  explicit CpuDevice(int node);
  CpuDevice(const CpuDevice&) = delete;
  CpuDevice& operator=(const CpuDevice&) = delete;
  CpuDevice(CpuDevice&&) = delete;
  CpuDevice& operator=(CpuDevice&&) = delete;
  ~CpuDevice() override;

  [[nodiscard]] std::string_view name() const override;

  // Fails with kUnavailable, with Embree's error, when Embree cannot build
  // a BVH (memory runs out, say).
  [[nodiscard]] std::expected<void, core::Error> Commit(
      const device::ChangeSet& changes) override;

  // What `ray` meets first, at its time, among the prototypes and the
  // shaped lights committed; none when it meets nothing, or after a commit
  // failed. Threads may call it at once, but not while a commit runs.
  [[nodiscard]] std::optional<Hit> Intersect(const Ray& ray) const;

  // Its arena, its copies of the prims and its BVHs.
  struct State;

 private:
  std::unique_ptr<State> state_;
};

}  // namespace umbra::ext::embree

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_EMBREE_CPU_DEVICE_H_
