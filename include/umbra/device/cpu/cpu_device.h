// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// A CPU device: one NUMA node, whose render data is Embree's BVHs, built in
// the node's own memory (docs/adr/0004-render-devices.md,
// docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_DEVICE_CPU_CPU_DEVICE_H_
#define UMBRA_DEVICE_CPU_CPU_DEVICE_H_

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/device/geometry.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device::cpu {

// A ray in world space, in meters. Its direction need not be of unit
// length: distances along it are in units of its direction.
struct Ray {
  device::Float3 origin;
  device::Float3 direction;
  // When it is cast, from 0 when the shutter opens to 1 when it closes.
  float time = 0.0F;
  // The nearest and the farthest distances it meets something at.
  float t_min = 0.0F;
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
  // The geometric normal there, in world space, of unit length, facing the
  // ray.
  device::Vector3 normal;
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
// It renders a product with a placeholder integrator: one tile per thread
// of its arena in each batch it takes, each sample of a pixel a primary
// ray, cast against its BVHs, and its pixels the mean of their samples, as
// the single-source placeholder pixel of umbra/device/film.h gives them:
// the color 0.5·d + 0.5 of the ray's world direction d, with an alpha of 1;
// the depth of the first hit, in meters, +∞ where there is none; its
// geometric normal in world space, facing the ray, 0 where there is none;
// and an albedo of 0. Its film, in its node's memory, holds at most
// kMaxFilmPlanes vars and grows to the largest product it renders, and its
// sampler's cache is initialized at its first render.
//
// kTopology rebuilds a prototype's BVH. kPoints refits it, but rebuilds it
// when it was not built to be refitted, as at the first change of its
// points, when the number of motion samples or of points changes, and after
// kMaxRefits refits in a row, as device::PlanAccelUpdate decides. It logs,
// as the device component at debug level, what each commit built, refitted
// and rebuilt.
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

  // Fails with kInternal when it holds no camera of `frame`'s id.
  [[nodiscard]] std::expected<void, core::Error> Render(
      const device::FrameParameters& frame, device::TileQueue& tiles) override;

  // What `ray` meets first, at its time, among the prototypes and the
  // shaped lights committed; none when it meets nothing, or after a commit
  // failed. Threads may call it at once, but not while a commit runs.
  [[nodiscard]] std::optional<Hit> Intersect(const Ray& ray) const
      [[clang::nonblocking]];

  // Its arena, its copies of the prims and its BVHs.
  struct State;

 private:
  std::unique_ptr<State> state_;
};

}  // namespace umbra::device::cpu

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_CPU_CPU_DEVICE_H_
