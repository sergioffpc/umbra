// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The Embree BVHs of a CPU device: one per prototype, one per light with a
// shape, and the top level that instances them.

#ifndef UMBRA_DEVICE_CPU_BVH_H_
#define UMBRA_DEVICE_CPU_BVH_H_

#include <embree4/rtcore_device.h>
#include <embree4/rtcore_scene.h>

#include <cstdint>
#include <expected>
#include <utility>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/cpu/cpu_device.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/render_data.h"
#include "umbra/ext/embree/owners.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device::cpu::internal {

using Status = std::expected<void, core::Error>;
using ::umbra::ext::embree::Scene;

// How a prototype's BVH is built.
enum class BvhKind : std::uint8_t {
  // For tracing alone, as static geometry is.
  kStatic,
  // To be refitted when its points change, at some cost in tracing.
  kRefittable,
};

// A BVH of `prototype`'s triangles, with one time step per motion sample of
// its points. Fails with kUnavailable, with Embree's error.
[[nodiscard]] std::expected<Scene, core::Error> BuildPrototype(
    RTCDevice device, const device::Prototype& prototype, BvhKind kind);

// Refits `scene`, a kRefittable BVH of `prototype`, to its points, which
// changed and are as many, with as many motion samples, as those it was
// built from.
[[nodiscard]] Status RefitPrototype(RTCDevice device, const Scene& scene,
                                    const device::Prototype& prototype);

// A BVH of one primitive, `shape`, which must outlive it; it only reads it.
[[nodiscard]] std::expected<Scene, core::Error> BuildLight(
    RTCDevice device, device::LightShape& shape);

// What a top-level instance places.
struct Placement {
  // A prototype's or a light's.
  HitKind kind = HitKind::kPrototype;
  std::uint32_t id = 0;
  // Which of its instances.
  std::uint32_t instance = 0;
};

// The top level being built: an instance of a BVH per placement.
class TopLevel {
 public:
  explicit TopLevel(RTCDevice device);

  // Places `bvh` with the transforms of `instance`.
  void Add(const Scene& bvh, const device::Instance& instance,
           const Placement& placement);

  // The top level, built, and what each of its instances places, by
  // instance id. Fails with kUnavailable, with Embree's error.
  [[nodiscard]] std::expected<std::pair<Scene, std::vector<Placement>>,
                              core::Error>
  Build() &&;

 private:
  RTCDevice device_;
  Scene scene_;
  std::vector<Placement> placements_;
};

}  // namespace umbra::device::cpu::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_CPU_BVH_H_
