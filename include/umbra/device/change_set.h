// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The change set: what the render delegate hands every device at a commit,
// the prims added, updated and removed since the last one, by id
// (docs/adr/0013-devices-render-from-their-own-memory.md). It is a view: its
// spans point into the host copies, which live until the commit returns.

#ifndef UMBRA_DEVICE_CHANGE_SET_H_
#define UMBRA_DEVICE_CHANGE_SET_H_

#include <cstdint>
#include <span>
#include <type_traits>

#include "umbra/core/safety.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device {

// What changed in an updated prototype, as flags. A device rebuilds or
// refits only what they name.
enum class ChangeFlags : std::uint8_t {
  kNone = 0,
  // Its instances, or their transforms, changed.
  kPlacement = 1U << 0U,
  // Its points changed, and its topology did not.
  kPoints = 1U << 1U,
  // Its triangles changed.
  kTopology = 1U << 2U,
  // Its normals or another primvar changed.
  kPrimvars = 1U << 3U,
  // Its material, or a material of one of its subsets, changed.
  kMaterials = 1U << 4U,
};

[[nodiscard]] constexpr ChangeFlags operator|(ChangeFlags lhs,
                                              ChangeFlags rhs) {
  using Bits = std::underlying_type_t<ChangeFlags>;
  return static_cast<ChangeFlags>(static_cast<Bits>(lhs) |
                                  static_cast<Bits>(rhs));
}

// Whether `flags` holds any of `wanted`.
[[nodiscard]] constexpr bool HasAny(ChangeFlags flags, ChangeFlags wanted) {
  using Bits = std::underlying_type_t<ChangeFlags>;
  return (static_cast<Bits>(flags) & static_cast<Bits>(wanted)) != 0;
}

// A prototype updated, whole, and what changed in it.
struct UpdatedPrototype {
  Prototype prototype;
  ChangeFlags changes = ChangeFlags::kNone;
};

// The prims of one kind added, updated and removed. A light, a material, a
// camera or a texture is updated whole.
template <typename View, typename Id>
struct Changes {
  std::span<const View> added;
  std::span<const View> updated;
  std::span<const Id> removed;
};

// The prototypes added, updated and removed.
struct PrototypeChanges {
  std::span<const Prototype> added;
  std::span<const UpdatedPrototype> updated;
  std::span<const PrototypeId> removed;
};

// The number of ids of each kind: every id in use is below it.
struct IdCounts {
  std::uint32_t prototypes = 0;
  std::uint32_t lights = 0;
  std::uint32_t materials = 0;
  std::uint32_t cameras = 0;
  std::uint32_t textures = 0;
};

// What changed since the last commit. Render data refers to prims of other
// kinds by id (a prototype to its materials, a material or a light to its
// textures): each refers to a prim the change set adds, or to one a device
// already holds that it does not remove.
struct ChangeSet {
  IdCounts ids;
  PrototypeChanges prototypes;
  Changes<Light, LightId> lights;
  Changes<MaterialNetwork, MaterialId> materials;
  Changes<Camera, CameraId> cameras;
  Changes<Texture, TextureId> textures;
};

}  // namespace umbra::device

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_CHANGE_SET_H_
