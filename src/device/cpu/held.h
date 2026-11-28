// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// What a CPU device holds of the prims committed to it, in the tables by id
// of umbra/device/id_table.h, in its node's memory: its copies of the
// prims, the BVHs built from them, and the top level that instances them
// (docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_DEVICE_CPU_HELD_H_
#define UMBRA_DEVICE_CPU_HELD_H_

#include <optional>
#include <vector>

#include "bvh.h"
#include "copies.h"
#include "umbra/core/safety.h"
#include "umbra/device/accel_update.h"
#include "umbra/device/id_table.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/render_data.h"
#include "umbra/ext/embree/owners.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device::cpu::internal {

// A prototype held: its copy, and its BVH.
struct PrototypeRecord {
  PrototypeCopy copy;
  ext::embree::Scene bvh;
  BvhKind kind = BvhKind::kStatic;
  // The refits in a row since it was built.
  int refits = 0;
  // What the last commit did to its BVH: none when it built it, as it
  // added the prototype.
  std::optional<device::AccelUpdate> event;
};

// A light held: its copy and, when rays meet it, its shape, which its BVH
// holds. Its BVH reads its shape where it is: the table it is held in
// keeps it there.
struct LightRecord {
  Copy<device::Light> copy;
  std::optional<device::LightShape> shape;
  ext::embree::Scene bvh;
};

// Everything a CPU device holds of the commits.
struct Held {
  device::IdTable<PrototypeRecord, device::PrototypeId> prototypes;
  device::IdTable<LightRecord, device::LightId> lights;
  device::IdTable<Copy<device::Camera>, device::CameraId> cameras;
  device::IdTable<Copy<device::MaterialNetwork>, device::MaterialId> materials;
  device::IdTable<Copy<device::Texture>, device::TextureId> textures;
  // The top level, and what each of its instances places.
  ext::embree::Scene top;
  std::vector<Placement> placements;
};

}  // namespace umbra::device::cpu::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_CPU_HELD_H_
