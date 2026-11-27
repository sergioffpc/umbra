// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "copies.h"

#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "umbra/device/change_set.h"
#include "umbra/device/render_data.h"

namespace umbra::ext::embree::internal {
namespace {

using ::umbra::device::ChangeFlags;
using ::umbra::device::HasAny;

device::Primvar CopyPrimvar(const device::Primvar& primvar, Storage& storage) {
  return {
      .name = storage.Keep(primvar.name),
      .rate = primvar.rate,
      .components = primvar.components,
      .floats = storage.Keep(primvar.floats),
      .integers = storage.Keep(primvar.integers),
      .indices = storage.Keep(primvar.indices),
  };
}

std::span<const device::Primvar> CopyPrimvars(
    std::span<const device::Primvar> primvars, Storage& storage) {
  std::vector<device::Primvar> copies;
  copies.reserve(primvars.size());
  for (const device::Primvar& primvar : primvars) {
    copies.push_back(CopyPrimvar(primvar, storage));
  }
  return storage.Own(std::move(copies));
}

std::span<const device::Instance> CopyInstances(
    std::span<const device::Instance> instances, Storage& storage) {
  std::vector<device::Instance> copies;
  copies.reserve(instances.size());
  for (const device::Instance& instance : instances) {
    copies.push_back({
        .object_to_world = storage.Keep(instance.object_to_world),
        .primvars = CopyPrimvars(instance.primvars, storage),
    });
  }
  return storage.Own(std::move(copies));
}

std::span<const std::span<const device::Float3>> CopyPoints(
    std::span<const std::span<const device::Float3>> samples,
    Storage& storage) {
  std::vector<std::span<const device::Float3>> copies;
  copies.reserve(samples.size());
  for (const std::span<const device::Float3> sample : samples) {
    copies.push_back(storage.Keep(sample));
  }
  return storage.Own(std::move(copies));
}

void CopyTopology(const device::Prototype& prototype, PrototypeCopy& copy) {
  copy.topology.Clear();
  copy.view.triangles = copy.topology.Keep(prototype.triangles);
  copy.view.source_faces = copy.topology.Keep(prototype.source_faces);
  copy.view.face_vertex_counts =
      copy.topology.Keep(prototype.face_vertex_counts);
  copy.view.sidedness = prototype.sidedness;
}

void CopyPointsOf(const device::Prototype& prototype, PrototypeCopy& copy) {
  copy.points.Clear();
  copy.view.points = CopyPoints(prototype.points, copy.points);
}

void CopyPlacement(const device::Prototype& prototype, PrototypeCopy& copy) {
  copy.placement.Clear();
  copy.view.instances = CopyInstances(prototype.instances, copy.placement);
}

void CopyPrimvarsOf(const device::Prototype& prototype, PrototypeCopy& copy) {
  copy.primvars.Clear();
  copy.view.normals = CopyPrimvar(prototype.normals, copy.primvars);
  copy.view.primvars = CopyPrimvars(prototype.primvars, copy.primvars);
}

void CopyMaterials(const device::Prototype& prototype, PrototypeCopy& copy) {
  copy.materials.Clear();
  copy.view.materials = copy.materials.Keep(prototype.materials);
  copy.view.triangle_materials =
      copy.materials.Keep(prototype.triangle_materials);
}

std::span<const std::string_view> CopyTexts(
    std::span<const std::string_view> texts, Storage& storage) {
  std::vector<std::string_view> copies;
  copies.reserve(texts.size());
  for (const std::string_view text : texts) {
    copies.push_back(storage.Keep(text));
  }
  return storage.Own(std::move(copies));
}

template <typename T>
inline constexpr bool kIsSpan = false;
template <typename T>
inline constexpr bool kIsSpan<std::span<const T>> = true;

device::ParameterValue CopyValue(const device::ParameterValue& value,
                                 Storage& storage) {
  return std::visit(
      [&storage](const auto& alternative) -> device::ParameterValue {
        using T = std::decay_t<decltype(alternative)>;
        if constexpr (std::is_same_v<T, std::span<const std::string_view>>) {
          return CopyTexts(alternative, storage);
        } else if constexpr (std::is_same_v<T, std::string_view> ||
                             kIsSpan<T>) {
          return storage.Keep(alternative);
        } else {
          return alternative;
        }
      },
      value);
}

std::span<const device::Parameter> CopyParameters(
    std::span<const device::Parameter> parameters, Storage& storage) {
  std::vector<device::Parameter> copies;
  copies.reserve(parameters.size());
  for (const device::Parameter& parameter : parameters) {
    copies.push_back({
        .name = storage.Keep(parameter.name),
        .value = CopyValue(parameter.value, storage),
    });
  }
  return storage.Own(std::move(copies));
}

std::span<const device::Connection> CopyConnections(
    std::span<const device::Connection> connections, Storage& storage) {
  std::vector<device::Connection> copies;
  copies.reserve(connections.size());
  for (const device::Connection& connection : connections) {
    copies.push_back({
        .input = storage.Keep(connection.input),
        .node = connection.node,
        .output = storage.Keep(connection.output),
    });
  }
  return storage.Own(std::move(copies));
}

std::span<const device::MaterialNode> CopyNodes(
    std::span<const device::MaterialNode> nodes, Storage& storage) {
  std::vector<device::MaterialNode> copies;
  copies.reserve(nodes.size());
  for (const device::MaterialNode& node : nodes) {
    copies.push_back({
        .name = storage.Keep(node.name),
        .kind = node.kind,
        .shader = storage.Keep(node.shader),
        .parameters = CopyParameters(node.parameters, storage),
        .connections = CopyConnections(node.connections, storage),
    });
  }
  return storage.Own(std::move(copies));
}

}  // namespace

std::string_view Storage::Keep(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const std::span<const char> kept =
      Own(std::vector<char>(text.begin(), text.end()));
  return {kept.data(), kept.size()};
}

void Storage::Clear() {
  std::apply([](auto&... pools) { (pools.clear(), ...); }, pools_);
}

void CopyPrototype(const device::Prototype& prototype, PrototypeCopy& copy) {
  copy.path.Clear();
  copy.view.id = prototype.id;
  copy.view.path = copy.path.Keep(prototype.path);
  CopyPointsOf(prototype, copy);
  CopyTopology(prototype, copy);
  CopyPrimvarsOf(prototype, copy);
  CopyMaterials(prototype, copy);
  CopyPlacement(prototype, copy);
  copy.view.emission = prototype.emission;
}

void UpdatePrototype(const device::UpdatedPrototype& updated,
                     PrototypeCopy& copy) {
  const device::Prototype& prototype = updated.prototype;
  if (HasAny(updated.changes, ChangeFlags::kTopology)) {
    CopyTopology(prototype, copy);
  }
  if (HasAny(updated.changes, ChangeFlags::kTopology | ChangeFlags::kPoints)) {
    CopyPointsOf(prototype, copy);
  }
  if (HasAny(updated.changes, ChangeFlags::kPrimvars)) {
    CopyPrimvarsOf(prototype, copy);
  }
  if (HasAny(updated.changes, ChangeFlags::kMaterials)) {
    CopyMaterials(prototype, copy);
  }
  if (HasAny(updated.changes, ChangeFlags::kPlacement)) {
    CopyPlacement(prototype, copy);
  }
  if (HasAny(updated.changes, ChangeFlags::kEmission)) {
    copy.view.emission = prototype.emission;
  }
}

device::Light CopyOf(const device::Light& light, Storage& storage) {
  device::Light copy = light;
  copy.path = storage.Keep(light.path);
  copy.instances = CopyInstances(light.instances, storage);
  copy.tilt_factors = storage.Keep(light.tilt_factors);
  if (auto* dome = std::get_if<device::Light::Dome>(&copy.shape)) {
    dome->portals = storage.Keep(dome->portals);
  }
  if (copy.ies_profile.has_value()) {
    device::IesProfile& profile = *copy.ies_profile;
    profile.vertical_angles = storage.Keep(profile.vertical_angles);
    profile.horizontal_angles = storage.Keep(profile.horizontal_angles);
    profile.candelas = storage.Keep(profile.candelas);
  }
  return copy;
}

device::Camera CopyOf(const device::Camera& camera, Storage& storage) {
  device::Camera copy = camera;
  copy.path = storage.Keep(camera.path);
  copy.camera_to_world = storage.Keep(camera.camera_to_world);
  return copy;
}

device::MaterialNetwork CopyOf(const device::MaterialNetwork& material,
                               Storage& storage) {
  return {
      .id = material.id,
      .path = storage.Keep(material.path),
      .nodes = CopyNodes(material.nodes, storage),
      .surface =
          {
              .node = material.surface.node,
              .name = storage.Keep(material.surface.name),
          },
  };
}

device::Texture CopyOf(const device::Texture& texture, Storage& storage) {
  return {
      .id = texture.id,
      .file = storage.Keep(texture.file),
      .color_space = storage.Keep(texture.color_space),
  };
}

}  // namespace umbra::ext::embree::internal
