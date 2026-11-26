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

#include "umbra/core/safety.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device {

// What changed in an updated prototype, as flags. A device rebuilds or
// refits only what they name.
class ChangeFlags {
 public:
  // Nothing.
  static const ChangeFlags kNone;
  // Its instances, or their transforms, changed.
  static const ChangeFlags kPlacement;
  // Its points changed, and its topology did not.
  static const ChangeFlags kPoints;
  // Its triangles changed, and its points with them. It comes with
  // kPrimvars and kMaterials, whose values follow the triangles.
  static const ChangeFlags kTopology;
  // Its normals or another primvar changed.
  static const ChangeFlags kPrimvars;
  // Its material, or a material of one of its subsets, changed.
  static const ChangeFlags kMaterials;

  constexpr ChangeFlags() = default;

  [[nodiscard]] friend constexpr ChangeFlags operator|(ChangeFlags lhs,
                                                       ChangeFlags rhs) {
    return ChangeFlags(static_cast<Bits>(lhs.bits_ | rhs.bits_));
  }
  [[nodiscard]] friend constexpr bool operator==(ChangeFlags lhs,
                                                 ChangeFlags rhs) = default;

  friend constexpr bool HasAny(ChangeFlags flags, ChangeFlags wanted);

 private:
  using Bits = std::uint8_t;

  constexpr explicit ChangeFlags(Bits bits) : bits_(bits) {}

  Bits bits_ = 0;
};

inline constexpr ChangeFlags ChangeFlags::kNone{};
inline constexpr ChangeFlags ChangeFlags::kPlacement{1U << 0U};
inline constexpr ChangeFlags ChangeFlags::kPoints{1U << 1U};
inline constexpr ChangeFlags ChangeFlags::kTopology{1U << 2U};
inline constexpr ChangeFlags ChangeFlags::kPrimvars{1U << 3U};
inline constexpr ChangeFlags ChangeFlags::kMaterials{1U << 4U};

// Whether `flags` holds any of `wanted`.
[[nodiscard]] constexpr bool HasAny(ChangeFlags flags, ChangeFlags wanted) {
  return (flags.bits_ & wanted.bits_) != 0U;
}

// A prototype updated, and what changed in it. Its id and its path are
// given, and the members its flags name; the others are empty:
//
// - kPlacement: instances;
// - kPoints: points;
// - kTopology: points, triangles, source_faces, face_vertex_counts and
//   sidedness;
// - kPrimvars: normals and primvars;
// - kMaterials: materials and triangle_materials.
//
// A device keeps the rest of its copy of the prototype.
struct UpdatedPrototype {
  Prototype prototype;
  ChangeFlags changes = ChangeFlags::kNone;
};

// The prims of one kind added, updated and removed. A light, a material or
// a camera is updated whole. A texture is never updated: it is added when a
// material or a light first reads its file, and removed when none does.
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

// What changed since the last commit. A device applies its removals first,
// then its updates and its additions; an id removed is free, and may be
// added again at a later commit. Render data refers to prims of other
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
