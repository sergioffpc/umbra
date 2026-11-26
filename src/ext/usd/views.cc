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
#include "umbra/device/change_set.h"
#include "umbra/device/render_data.h"

namespace umbra::ext::usd::internal {
namespace {

// The next id of a kind of which `views` are added.
template <typename Id, typename View>
Id NextId(const std::vector<View>& views) {
  return static_cast<Id>(views.size());
}

// `index`, an index into an array of a host copy, as an index of a view.
std::uint32_t IndexOf(std::size_t index) {
  return static_cast<std::uint32_t>(index);
}

}  // namespace

device::TextureId ChangeSetBuilder::TextureIdOf(const host::Texture& texture) {
  const std::pair<std::string_view, std::string_view> key = {
      texture.file, texture.color_space};
  if (const auto found = texture_ids_.find(key); found != texture_ids_.end()) {
    return found->second;
  }
  const auto texture_id = NextId<device::TextureId>(textures_);
  textures_.push_back({
      .id = texture_id,
      .file = texture.file,
      .color_space = texture.color_space,
  });
  texture_ids_.emplace(key, texture_id);
  return texture_id;
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
  const auto texture = [this](const std::optional<host::Texture>& texture)
      -> std::optional<device::TextureId> {
    if (!texture.has_value()) {
      return std::nullopt;
    }
    return TextureIdOf(*texture);
  };
  return std::visit(
      [&]<typename T>(const T& held) -> device::Light::Shape {
        if constexpr (std::is_same_v<T, host::Light::Sphere>) {
          return device::Light::Sphere{.radius = held.radius};
        } else if constexpr (std::is_same_v<T, host::Light::Rect>) {
          return device::Light::Rect{
              .width = held.width,
              .height = held.height,
              .texture = texture(held.texture),
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
        } else {
          static_assert(std::is_same_v<T, host::Light::Dome>);
          return device::Light::Dome{
              .texture = texture(held.texture),
              .format = held.format,
              .orientation = held.orientation,
          };
        }
      },
      shape);
}

device::CameraId ChangeSetBuilder::AddCamera(const host::Camera& camera) {
  const auto camera_id = NextId<device::CameraId>(cameras_);
  cameras_.push_back({
      .id = camera_id,
      .path = camera.path,
      .projection = camera.projection,
      .camera_to_world = camera.camera_to_world,
      .focal_length = camera.focal_length,
      .aperture = camera.aperture,
      .aperture_offset = camera.aperture_offset,
      .clipping_range = camera.clipping_range,
      .f_stop = camera.f_stop,
      .focus_distance = camera.focus_distance,
  });
  return camera_id;
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

device::MaterialId ChangeSetBuilder::AddMaterial(
    const host::MaterialNetwork& network) {
  std::vector<device::MaterialNode> nodes;
  nodes.reserve(network.nodes.size());
  for (const host::MaterialNode& node : network.nodes) {
    nodes.push_back(NodeOf(node));
  }
  const auto material_id = NextId<device::MaterialId>(materials_);
  materials_.push_back({
      .id = material_id,
      .path = network.path,
      .nodes = Keep(std::move(nodes)),
      .surface =
          {
              .node = IndexOf(network.surface.node),
              .name = network.surface.name,
          },
  });
  return material_id;
}

device::PrototypeId ChangeSetBuilder::AddPrototype(
    const host::Mesh& mesh, std::span<const host::Instance> instances) {
  std::vector<std::span<const device::Float3>> points;
  points.reserve(mesh.points.size());
  for (const host::HostBuffer<device::Float3>& sample : mesh.points) {
    points.emplace_back(sample);
  }
  const auto prototype_id = NextId<device::PrototypeId>(prototypes_);
  prototypes_.push_back({
      .id = prototype_id,
      .path = mesh.path,
      .points = Keep(std::move(points)),
      .triangles = mesh.triangles,
      .source_faces = mesh.source_faces,
      .face_vertex_counts = mesh.face_vertex_counts,
      .normals = PrimvarOf(mesh.normals),
      .primvars = PrimvarsOf(mesh.primvars),
      .sidedness = mesh.sidedness,
      .materials = mesh.materials,
      .triangle_materials = mesh.triangle_materials,
      .instances = InstancesOf(instances),
  });
  return prototype_id;
}

device::LightId ChangeSetBuilder::AddLight(const host::Light& light) {
  const auto light_id = NextId<device::LightId>(lights_);
  lights_.push_back({
      .id = light_id,
      .path = light.path,
      .shape = ShapeOf(light.shape),
      .intensity = light.intensity,
      .exposure = light.exposure,
      .normalization = light.normalization,
      .color = light.color,
      .color_temperature = light.color_temperature,
      .cone_angle = light.cone_angle,
      .instances = InstancesOf(light.instances),
  });
  return light_id;
}

device::ChangeSet ChangeSetBuilder::Build() const {
  return {
      .ids =
          {
              .prototypes = IndexOf(prototypes_.size()),
              .lights = IndexOf(lights_.size()),
              .materials = IndexOf(materials_.size()),
              .cameras = IndexOf(cameras_.size()),
              .textures = IndexOf(textures_.size()),
          },
      .prototypes = {.added = prototypes_, .updated = {}, .removed = {}},
      .lights = {.added = lights_, .updated = {}, .removed = {}},
      .materials = {.added = materials_, .updated = {}, .removed = {}},
      .cameras = {.added = cameras_, .updated = {}, .removed = {}},
      .textures = {.added = textures_, .updated = {}, .removed = {}},
  };
}

}  // namespace umbra::ext::usd::internal
