// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "views.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "host_copy.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/render_data.h"

namespace umbra::delegate::internal {
namespace {

// `index`, an index into an array of a host copy, as an index of a view.
std::uint32_t IndexOf(std::size_t index) {
  return static_cast<std::uint32_t>(index);
}

// The view of the IES profile `profile`, if any.
std::optional<device::IesProfile> IesProfileOf(
    const std::optional<host::IesProfile>& profile UMBRA_LIFETIMEBOUND) {
  if (!profile.has_value()) {
    return std::nullopt;
  }
  return device::IesProfile{
      .vertical_angles = profile->vertical_angles,
      .horizontal_angles = profile->horizontal_angles,
      .candelas = profile->candelas,
      .power = profile->power,
      .normalization = profile->normalization,
  };
}

}  // namespace

device::TextureId ChangeSetBuilder::TextureIdOf(
    const host::Texture& texture) const {
  return textures_->at(texture);
}

std::optional<device::TextureId> ChangeSetBuilder::TextureIdOf(
    const std::optional<host::Texture>& texture) const {
  if (!texture.has_value()) {
    return std::nullopt;
  }
  return TextureIdOf(*texture);
}

device::Primvar ChangeSetBuilder::PrimvarOf(const host::Primvar& primvar) {
  device::Primvar view{
      .name = primvar.name,
      .rate = primvar.rate,
      .components = primvar.components,
      .floats = {},
      .integers = {},
      .indices = primvar.indices,
  };
  if (const auto* floats =
          std::get_if<host::HostBuffer<float>>(&primvar.values)) {
    view.floats = *floats;
  } else if (const auto* integers =
                 std::get_if<host::HostBuffer<std::int32_t>>(&primvar.values)) {
    view.integers = *integers;
  }
  return view;
}

std::span<const device::Primvar> ChangeSetBuilder::PrimvarsOf(
    const std::vector<host::Primvar>& primvars) {
  if (primvars.empty()) {
    return {};
  }
  std::vector<device::Primvar> views;
  views.reserve(primvars.size());
  for (const host::Primvar& primvar : primvars) {
    views.push_back(PrimvarOf(primvar));
  }
  return Keep(std::move(views));
}

std::span<const device::Instance> ChangeSetBuilder::InstancesOf(
    std::span<const host::Instance> instances) {
  std::vector<device::Instance> views;
  views.reserve(instances.size());
  for (const host::Instance& instance : instances) {
    views.push_back({
        .object_to_world = instance.object_to_world,
        .primvars = PrimvarsOf(instance.primvars),
    });
  }
  return Keep(std::move(views));
}

device::ParameterValue ChangeSetBuilder::ValueOf(
    const host::ParameterValue& value) {
  return std::visit(
      [this]<typename T>(const T& held) -> device::ParameterValue {
        if constexpr (std::is_same_v<T, std::string>) {
          return std::string_view(held);
        } else if constexpr (std::is_same_v<T, host::Texture>) {
          return TextureIdOf(held);
        } else if constexpr (std::is_same_v<T,
                                            host::HostBuffer<std::int32_t>>) {
          return std::span<const std::int32_t>(held);
        } else if constexpr (std::is_same_v<T, host::HostBuffer<float>>) {
          return std::span<const float>(held);
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
          return Keep(std::vector<std::string_view>(held.begin(), held.end()));
        } else {
          return held;
        }
      },
      value);
}

device::Light::Shape ChangeSetBuilder::ShapeOf(
    const host::Light::Shape& shape) {
  return std::visit(
      [&]<typename T>(const T& held) -> device::Light::Shape {
        if constexpr (std::is_same_v<T, host::Light::Sphere>) {
          return device::Light::Sphere{.radius = held.radius};
        } else if constexpr (std::is_same_v<T, host::Light::Rect>) {
          return device::Light::Rect{
              .width = held.width,
              .height = held.height,
              .texture = TextureIdOf(held.texture),
          };
        } else if constexpr (std::is_same_v<T, host::Light::Disk>) {
          return device::Light::Disk{.radius = held.radius};
        } else if constexpr (std::is_same_v<T, host::Light::Cylinder>) {
          return device::Light::Cylinder{
              .length = held.length,
              .radius = held.radius,
          };
        } else if constexpr (std::is_same_v<T, host::Light::Distant>) {
          return device::Light::Distant{.angle = held.angle};
        } else if constexpr (std::is_same_v<T, host::Light::Dome>) {
          return device::Light::Dome{
              .texture = TextureIdOf(held.texture),
              .format = held.format,
              .orientation = held.orientation,
              .portals = held.portals,
          };
        } else {
          static_assert(std::is_same_v<T, host::Light::Portal>);
          return held;
        }
      },
      shape);
}

device::Camera ChangeSetBuilder::CameraOf(device::CameraId prim_id,
                                          const host::Camera& camera) {
  return {
      .id = prim_id,
      .path = camera.path,
      .projection = camera.projection,
      .camera_to_world = camera.camera_to_world,
      .focal_length = camera.focal_length,
      .aperture = camera.aperture,
      .aperture_offset = camera.aperture_offset,
      .clipping_range = camera.clipping_range,
      .f_stop = camera.f_stop,
      .focus_distance = camera.focus_distance,
      .exposure_scale = camera.exposure_scale,
  };
}

device::MaterialNode ChangeSetBuilder::NodeOf(const host::MaterialNode& node) {
  std::vector<device::Parameter> parameters;
  parameters.reserve(node.parameters.size());
  for (const host::Parameter& parameter : node.parameters) {
    parameters.push_back({
        .name = parameter.name,
        .value = ValueOf(parameter.value),
    });
  }
  std::vector<device::Connection> connections;
  connections.reserve(node.connections.size());
  for (const host::Connection& connection : node.connections) {
    connections.push_back({
        .input = connection.input,
        .node = IndexOf(connection.node),
        .output = connection.output,
    });
  }
  return {
      .name = node.name,
      .kind = node.kind,
      .shader = node.shader,
      .parameters = Keep(std::move(parameters)),
      .connections = Keep(std::move(connections)),
  };
}

device::MaterialNetwork ChangeSetBuilder::MaterialOf(
    device::MaterialId prim_id, const host::MaterialNetwork& network) {
  std::vector<device::MaterialNode> nodes;
  nodes.reserve(network.nodes.size());
  for (const host::MaterialNode& node : network.nodes) {
    nodes.push_back(NodeOf(node));
  }
  return {
      .id = prim_id,
      .path = network.path,
      .nodes = Keep(std::move(nodes)),
      .surface =
          {
              .node = IndexOf(network.surface.node),
              .name = network.surface.name,
          },
  };
}

device::Light ChangeSetBuilder::LightOf(device::LightId prim_id,
                                        const host::Light& light) {
  return {
      .id = prim_id,
      .path = light.path,
      .shape = ShapeOf(light.shape),
      .intensity = light.intensity,
      .exposure = light.exposure,
      .normalization = light.normalization,
      .color = light.color,
      .color_temperature = light.color_temperature,
      .cone_angle = light.cone_angle,
      .ies_profile = IesProfileOf(light.ies_profile),
      .instances = InstancesOf(light.instances),
      .tilt_factors = light.tilt_factors,
  };
}

std::span<const std::span<const device::Float3>> ChangeSetBuilder::PointsOf(
    const std::vector<host::HostBuffer<device::Float3>>& samples) {
  std::vector<std::span<const device::Float3>> points;
  points.reserve(samples.size());
  for (const host::HostBuffer<device::Float3>& sample : samples) {
    points.emplace_back(sample);
  }
  return Keep(std::move(points));
}

device::Prototype ChangeSetBuilder::PrototypeOf(
    device::PrototypeId prim_id, const host::Mesh& mesh,
    std::span<const host::Instance> instances, device::ChangeFlags changes) {
  using device::ChangeFlags;
  using device::HasAny;
  device::Prototype prototype{
      .id = prim_id,
      .path = mesh.path,
      .points = {},
      .triangles = {},
      .source_faces = {},
      .face_vertex_counts = {},
      .normals = {},
      .primvars = {},
      .sidedness = device::Sidedness::kSingleSided,
      .materials = {},
      .triangle_materials = {},
      .instances = HasAny(changes, ChangeFlags::kPlacement)
                       ? InstancesOf(instances)
                       : std::span<const device::Instance>(),
      .emission = HasAny(changes, ChangeFlags::kEmission) ? mesh.emission
                                                          : std::nullopt,
  };
  if (HasAny(changes, ChangeFlags::kPoints | ChangeFlags::kTopology)) {
    prototype.points = PointsOf(mesh.points);
  }
  if (HasAny(changes, ChangeFlags::kTopology)) {
    prototype.triangles = mesh.triangles;
    prototype.source_faces = mesh.source_faces;
    prototype.face_vertex_counts = mesh.face_vertex_counts;
    prototype.sidedness = mesh.sidedness;
  }
  if (HasAny(changes, ChangeFlags::kPrimvars)) {
    prototype.normals = PrimvarOf(mesh.normals);
    prototype.primvars = PrimvarsOf(mesh.primvars);
  }
  if (HasAny(changes, ChangeFlags::kMaterials)) {
    prototype.materials = mesh.materials;
    prototype.triangle_materials = mesh.triangle_materials;
  }
  return prototype;
}

void ChangeSetBuilder::AddTexture(device::TextureId prim_id,
                                  const host::Texture& texture) {
  texture_changes_.added.push_back({
      .id = prim_id,
      .file = texture.file,
      .color_space = texture.color_space,
  });
}

void ChangeSetBuilder::RemoveTexture(device::TextureId prim_id) {
  texture_changes_.removed.push_back(prim_id);
}

void ChangeSetBuilder::AddCamera(device::CameraId prim_id,
                                 const host::Camera& camera) {
  cameras_.added.push_back(CameraOf(prim_id, camera));
}

void ChangeSetBuilder::UpdateCamera(device::CameraId prim_id,
                                    const host::Camera& camera) {
  cameras_.updated.push_back(CameraOf(prim_id, camera));
}

void ChangeSetBuilder::RemoveCamera(device::CameraId prim_id) {
  cameras_.removed.push_back(prim_id);
}

void ChangeSetBuilder::AddMaterial(device::MaterialId prim_id,
                                   const host::MaterialNetwork& network) {
  materials_.added.push_back(MaterialOf(prim_id, network));
}

void ChangeSetBuilder::UpdateMaterial(device::MaterialId prim_id,
                                      const host::MaterialNetwork& network) {
  materials_.updated.push_back(MaterialOf(prim_id, network));
}

void ChangeSetBuilder::RemoveMaterial(device::MaterialId prim_id) {
  materials_.removed.push_back(prim_id);
}

void ChangeSetBuilder::AddLight(device::LightId prim_id,
                                const host::Light& light) {
  lights_.added.push_back(LightOf(prim_id, light));
}

void ChangeSetBuilder::UpdateLight(device::LightId prim_id,
                                   const host::Light& light) {
  lights_.updated.push_back(LightOf(prim_id, light));
}

void ChangeSetBuilder::RemoveLight(device::LightId prim_id) {
  lights_.removed.push_back(prim_id);
}

void ChangeSetBuilder::AddPrototype(device::PrototypeId prim_id,
                                    const host::Mesh& mesh,
                                    std::span<const host::Instance> instances) {
  added_prototypes_.push_back(PrototypeOf(
      prim_id, mesh, instances,
      device::ChangeFlags::kPlacement | device::ChangeFlags::kTopology |
          device::ChangeFlags::kPrimvars | device::ChangeFlags::kMaterials |
          device::ChangeFlags::kEmission));
}

void ChangeSetBuilder::UpdatePrototype(
    device::PrototypeId prim_id, const host::Mesh& mesh,
    std::span<const host::Instance> instances, device::ChangeFlags changes) {
  updated_prototypes_.push_back({
      .prototype = PrototypeOf(prim_id, mesh, instances, changes),
      .changes = changes,
  });
}

void ChangeSetBuilder::RemovePrototype(device::PrototypeId prim_id) {
  removed_prototypes_.push_back(prim_id);
}

device::ChangeSet ChangeSetBuilder::Build(const device::IdCounts& ids) const {
  return {
      .ids = ids,
      .prototypes =
          {
              .added = added_prototypes_,
              .updated = updated_prototypes_,
              .removed = removed_prototypes_,
          },
      .lights =
          {
              .added = lights_.added,
              .updated = lights_.updated,
              .removed = lights_.removed,
          },
      .materials =
          {
              .added = materials_.added,
              .updated = materials_.updated,
              .removed = materials_.removed,
          },
      .cameras =
          {
              .added = cameras_.added,
              .updated = cameras_.updated,
              .removed = cameras_.removed,
          },
      .textures =
          {
              .added = texture_changes_.added,
              .updated = texture_changes_.updated,
              .removed = texture_changes_.removed,
          },
  };
}

}  // namespace umbra::delegate::internal
