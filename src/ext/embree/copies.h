// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// A CPU device's own copies of the prims a change set gives: views of the
// same types as the change set's, which point into memory the device owns,
// allocated by the thread that copies them, on the device's NUMA node.

#ifndef UMBRA_EXT_EMBREE_COPIES_H_
#define UMBRA_EXT_EMBREE_COPIES_H_

#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::embree::internal {

// Memory that holds copies of arrays: each stays where it is, so that the
// views into it stay valid, until the storage is cleared or destroyed.
class UMBRA_OWNER Storage {
 public:
  // A copy of `values`.
  template <typename T>
  [[nodiscard]] std::span<const T> Keep(std::span<const T> values)
      UMBRA_LIFETIMEBOUND {
    if (values.empty()) {
      return {};
    }
    return Own(std::vector<T>(values.begin(), values.end()));
  }

  // A copy of `text`.
  [[nodiscard]] std::string_view Keep(std::string_view text)
      UMBRA_LIFETIMEBOUND;

  // `values`, which it takes.
  template <typename T>
  [[nodiscard]] std::span<const T> Own(std::vector<T> values)
      UMBRA_LIFETIMEBOUND {
    // A vector's elements stay where they are when it moves.
    return std::get<Pool<T>>(pools_).emplace_back(std::move(values));
  }

  // Frees every copy.
  void Clear();

 private:
  template <typename T>
  using Pool = std::vector<std::vector<T>>;

  std::tuple<Pool<char>, Pool<device::Float3>, Pool<device::Triangle>,
             Pool<std::uint32_t>, Pool<std::int32_t>, Pool<float>,
             Pool<device::Matrix>, Pool<device::Primvar>,
             Pool<device::Instance>, Pool<std::span<const device::Float3>>,
             Pool<device::MaterialId>, Pool<device::MaterialNode>,
             Pool<device::Parameter>, Pool<device::Connection>,
             Pool<std::string_view>>
      pools_;
};

// A prototype, copied, with the storage of each group of members an update
// replaces together.
struct PrototypeCopy {
  device::Prototype view;
  // Its path.
  Storage path;
  // kPlacement: its instances.
  Storage placement;
  // kPoints: its points.
  Storage points;
  // kTopology: its triangles, source faces and face vertex counts.
  Storage topology;
  // kPrimvars: its normals and primvars.
  Storage primvars;
  // kMaterials: its materials and the material of each triangle.
  Storage materials;
};

// A light, a camera, a material network or a texture, copied whole.
template <typename View>
struct Copy {
  View view;
  Storage storage;
};

// Copies `prototype` into `copy`, every member.
void CopyPrototype(const device::Prototype& prototype, PrototypeCopy& copy);

// Applies `updated` to `copy`: copies the members its flags name, and keeps
// the others.
void UpdatePrototype(const device::UpdatedPrototype& updated,
                     PrototypeCopy& copy);

// Copies of a light, a camera, a material network and a texture, into
// `storage`.
[[nodiscard]] device::Light CopyOf(const device::Light& light,
                                   Storage& storage UMBRA_LIFETIMEBOUND);
[[nodiscard]] device::Camera CopyOf(const device::Camera& camera,
                                    Storage& storage UMBRA_LIFETIMEBOUND);
[[nodiscard]] device::MaterialNetwork CopyOf(
    const device::MaterialNetwork& material,
    Storage& storage UMBRA_LIFETIMEBOUND);
[[nodiscard]] device::Texture CopyOf(const device::Texture& texture,
                                     Storage& storage UMBRA_LIFETIMEBOUND);

}  // namespace umbra::ext::embree::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_EMBREE_COPIES_H_
