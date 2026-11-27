// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The change set of a commit, as views of the host copies
// (docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_EXT_USD_VIEWS_H_
#define UMBRA_EXT_USD_VIEWS_H_

#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "host_copy.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd::internal {

// The ids of the textures held, by file and color space.
using TextureIds = std::map<host::Texture, device::TextureId>;

// The prims of one kind a change set adds, updates and removes.
template <typename View, typename Id>
struct ChangeLists {
  std::vector<View> added;
  std::vector<View> updated;
  std::vector<Id> removed;
};

// Builds the change set of a commit: views of the host copies it is given,
// which must outlive it, under the ids the commit gives them, and the
// arrays of views they need, which it owns. A material or a light refers to
// its textures by the ids `textures` gives them, which must hold them when
// it is added or updated.
class ChangeSetBuilder {
 public:
  explicit ChangeSetBuilder(const TextureIds& textures UMBRA_LIFETIMEBOUND)
      : textures_(&textures) {}
  ChangeSetBuilder(const ChangeSetBuilder&) = delete;
  ChangeSetBuilder& operator=(const ChangeSetBuilder&) = delete;
  ChangeSetBuilder(ChangeSetBuilder&&) = delete;
  ChangeSetBuilder& operator=(ChangeSetBuilder&&) = delete;
  ~ChangeSetBuilder() = default;

  void AddTexture(device::TextureId prim_id,
                  const host::Texture& texture UMBRA_LIFETIME_CAPTURE_BY_THIS);
  void RemoveTexture(device::TextureId prim_id);

  void AddCamera(device::CameraId prim_id,
                 const host::Camera& camera UMBRA_LIFETIME_CAPTURE_BY_THIS);
  void UpdateCamera(device::CameraId prim_id,
                    const host::Camera& camera UMBRA_LIFETIME_CAPTURE_BY_THIS);
  void RemoveCamera(device::CameraId prim_id);

  void AddMaterial(device::MaterialId prim_id,
                   const host::MaterialNetwork& network
                       UMBRA_LIFETIME_CAPTURE_BY_THIS);
  void UpdateMaterial(device::MaterialId prim_id,
                      const host::MaterialNetwork& network
                          UMBRA_LIFETIME_CAPTURE_BY_THIS);
  void RemoveMaterial(device::MaterialId prim_id);

  // Adds `light`, which has an instance or more.
  void AddLight(device::LightId prim_id,
                const host::Light& light UMBRA_LIFETIME_CAPTURE_BY_THIS);
  void UpdateLight(device::LightId prim_id,
                   const host::Light& light UMBRA_LIFETIME_CAPTURE_BY_THIS);
  void RemoveLight(device::LightId prim_id);

  // Adds `mesh`, with its `instances`.
  void AddPrototype(device::PrototypeId prim_id,
                    const host::Mesh& mesh UMBRA_LIFETIME_CAPTURE_BY_THIS,
                    std::span<const host::Instance> instances
                        UMBRA_LIFETIME_CAPTURE_BY_THIS);
  // Updates the members of `mesh`, and its `instances`, that `changes`
  // names.
  void UpdatePrototype(device::PrototypeId prim_id,
                       const host::Mesh& mesh UMBRA_LIFETIME_CAPTURE_BY_THIS,
                       std::span<const host::Instance> instances
                           UMBRA_LIFETIME_CAPTURE_BY_THIS,
                       device::ChangeFlags changes);
  void RemovePrototype(device::PrototypeId prim_id);

  // The change set, with the id counts `ids`, valid while the builder and
  // the host copies live.
  [[nodiscard]] device::ChangeSet Build(const device::IdCounts& ids) const
      UMBRA_LIFETIMEBOUND;

 private:
  // The arrays of views the builder keeps, one list of each type.
  template <typename... Ts>
  using KeptArrays = std::tuple<std::vector<std::vector<Ts>>...>;

  // `items`, kept until the builder is destroyed: moving an array keeps its
  // storage where it is.
  template <typename T>
  std::span<const T> Keep(std::vector<T> items) {
    auto& arrays = std::get<std::vector<std::vector<T>>>(kept_);
    arrays.push_back(std::move(items));
    return arrays.back();
  }

  // The id of `texture`, which `textures_` holds.
  [[nodiscard]] device::TextureId TextureIdOf(
      const host::Texture& texture) const;
  // The id of `texture`, if any.
  [[nodiscard]] std::optional<device::TextureId> TextureIdOf(
      const std::optional<host::Texture>& texture) const;
  [[nodiscard]] static device::Primvar PrimvarOf(
      const host::Primvar& primvar UMBRA_LIFETIMEBOUND);
  // The views of the motion samples of points `samples`.
  std::span<const std::span<const device::Float3>> PointsOf(
      const std::vector<host::HostBuffer<device::Float3>>& samples
          UMBRA_LIFETIMEBOUND);
  std::span<const device::Primvar> PrimvarsOf(
      const std::vector<host::Primvar>& primvars UMBRA_LIFETIMEBOUND);
  std::span<const device::Instance> InstancesOf(
      std::span<const host::Instance> instances UMBRA_LIFETIMEBOUND);
  device::ParameterValue ValueOf(
      const host::ParameterValue& value UMBRA_LIFETIMEBOUND);
  device::MaterialNode NodeOf(
      const host::MaterialNode& node UMBRA_LIFETIMEBOUND);
  device::Light::Shape ShapeOf(
      const host::Light::Shape& shape UMBRA_LIFETIMEBOUND);
  [[nodiscard]] static device::Camera CameraOf(
      device::CameraId prim_id, const host::Camera& camera UMBRA_LIFETIMEBOUND);
  device::MaterialNetwork MaterialOf(device::MaterialId prim_id,
                                     const host::MaterialNetwork& network
                                         UMBRA_LIFETIMEBOUND);
  device::Light LightOf(device::LightId prim_id,
                        const host::Light& light UMBRA_LIFETIMEBOUND);
  // The view of the members of `mesh` and of its `instances` that
  // `changes` names.
  device::Prototype PrototypeOf(
      device::PrototypeId prim_id, const host::Mesh& mesh UMBRA_LIFETIMEBOUND,
      std::span<const host::Instance> instances UMBRA_LIFETIMEBOUND,
      device::ChangeFlags changes);

  const TextureIds* textures_;
  KeptArrays<std::span<const device::Float3>, device::Primvar, device::Instance,
             device::Parameter, device::Connection, device::MaterialNode,
             std::string_view>
      kept_;
  std::vector<device::Prototype> added_prototypes_;
  std::vector<device::UpdatedPrototype> updated_prototypes_;
  std::vector<device::PrototypeId> removed_prototypes_;
  ChangeLists<device::Light, device::LightId> lights_;
  ChangeLists<device::MaterialNetwork, device::MaterialId> materials_;
  ChangeLists<device::Camera, device::CameraId> cameras_;
  ChangeLists<device::Texture, device::TextureId> texture_changes_;
};

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_VIEWS_H_
