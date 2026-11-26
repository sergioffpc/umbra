// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The change set of a commit, as views of the host copies
// (docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_EXT_USD_VIEWS_H_
#define UMBRA_EXT_USD_VIEWS_H_

#include <map>
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

// Builds the change set of a commit in which every prim is added, with the
// next id of its kind: views of the host copies it is given, which must
// outlive it, and the arrays of views they need, which it owns. The
// textures of the materials and lights are added once each, by file and
// color space.
class ChangeSetBuilder {
 public:
  ChangeSetBuilder() = default;
  ChangeSetBuilder(const ChangeSetBuilder&) = delete;
  ChangeSetBuilder& operator=(const ChangeSetBuilder&) = delete;
  ChangeSetBuilder(ChangeSetBuilder&&) = delete;
  ChangeSetBuilder& operator=(ChangeSetBuilder&&) = delete;
  ~ChangeSetBuilder() = default;

  device::CameraId AddCamera(const host::Camera& camera UMBRA_LIFETIMEBOUND);
  device::MaterialId AddMaterial(
      const host::MaterialNetwork& network UMBRA_LIFETIMEBOUND);
  // Adds `mesh`, whose materials are those added, with its `instances`.
  device::PrototypeId AddPrototype(const host::Mesh& mesh UMBRA_LIFETIMEBOUND,
                                   std::span<const host::Instance> instances
                                       UMBRA_LIFETIMEBOUND);
  // Adds `light`, which has an instance or more.
  device::LightId AddLight(const host::Light& light UMBRA_LIFETIMEBOUND);

  // The change set, valid while the builder and the host copies live.
  [[nodiscard]] device::ChangeSet Build() const UMBRA_LIFETIMEBOUND;

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

  // The id of `texture`, added when it is not yet.
  device::TextureId TextureIdOf(
      const host::Texture& texture UMBRA_LIFETIMEBOUND);
  [[nodiscard]] static device::Primvar PrimvarOf(
      const host::Primvar& primvar UMBRA_LIFETIMEBOUND);
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

  KeptArrays<std::span<const device::Float3>, device::Primvar, device::Instance,
             device::Parameter, device::Connection, device::MaterialNode,
             std::string_view>
      kept_;
  std::vector<device::Prototype> prototypes_;
  std::vector<device::Light> lights_;
  std::vector<device::MaterialNetwork> materials_;
  std::vector<device::Camera> cameras_;
  std::vector<device::Texture> textures_;
  std::map<std::pair<std::string_view, std::string_view>, device::TextureId>
      texture_ids_;
};

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_VIEWS_H_
