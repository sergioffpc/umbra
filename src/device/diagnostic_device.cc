// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/device/diagnostic_device.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/device/change_set.h"
#include "umbra/device/render_data.h"

namespace umbra::device {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

using Checked = std::expected<void, Error>;
using Held = std::vector<std::uint8_t>;

// The error of an inconsistency of the change set.
std::unexpected<Error> Inconsistent(std::string message) {
  return std::unexpected(Error(ErrorCode::kInternal, std::move(message)));
}

// Whether `numbers` starts at a multiple of kBufferAlignment; an empty array
// starts nowhere.
template <typename T>
bool IsAligned(std::span<const T> numbers) {
  return numbers.empty() ||
         std::bit_cast<std::uintptr_t>(numbers.data()) % kBufferAlignment == 0;
}

// Fails, naming `owner` and `what`, when `numbers` is not aligned.
template <typename T>
Checked CheckAligned(std::span<const T> numbers, std::string_view owner,
                     std::string_view what) {
  if (!IsAligned(numbers)) {
    return Inconsistent(std::format("{}: its {} are not aligned to {} bytes",
                                    owner, what, kBufferAlignment));
  }
  return {};
}

// Whether `held` holds the id `index`.
bool IsHeld(const Held& held, std::uint32_t index) {
  return index < held.size() && held.at(index) != 0;
}

// The ids a change set gives one kind.
struct KindIds {
  std::vector<std::uint32_t> added;
  std::vector<std::uint32_t> updated;
  std::vector<std::uint32_t> removed;
};

// The ids of `changes`.
template <typename View, typename Id>
KindIds IdsOf(const Changes<View, Id>& changes) {
  KindIds ids;
  for (const View& view : changes.added) {
    ids.added.push_back(std::to_underlying(view.id));
  }
  for (const View& view : changes.updated) {
    ids.updated.push_back(std::to_underlying(view.id));
  }
  for (const Id removed : changes.removed) {
    ids.removed.push_back(std::to_underlying(removed));
  }
  return ids;
}

KindIds IdsOf(const PrototypeChanges& changes) {
  KindIds ids;
  for (const Prototype& prototype : changes.added) {
    ids.added.push_back(std::to_underlying(prototype.id));
  }
  for (const UpdatedPrototype& updated : changes.updated) {
    ids.updated.push_back(std::to_underlying(updated.prototype.id));
  }
  for (const PrototypeId removed : changes.removed) {
    ids.removed.push_back(std::to_underlying(removed));
  }
  return ids;
}

// The error of the id `index` of `kind`, not below `count`.
std::unexpected<Error> OutOfRange(std::string_view kind, std::uint32_t index,
                                  std::uint32_t count) {
  return Inconsistent(std::format("{} {}: its id is not below their count, {}",
                                  kind, index, count));
}

// Unholds the ids `removed` in `held`, which must hold them.
Checked Remove(std::string_view kind, std::uint32_t count,
               std::span<const std::uint32_t> removed, Held& held) {
  for (const std::uint32_t index : removed) {
    if (index >= count) {
      return OutOfRange(kind, index, count);
    }
    if (!IsHeld(held, index)) {
      return Inconsistent(
          std::format("{} {}: removed, and not held", kind, index));
    }
    held.at(index) = 0;
  }
  return {};
}

// Checks that `held` holds the ids `updated`.
Checked CheckUpdated(std::string_view kind, std::uint32_t count,
                     std::span<const std::uint32_t> updated, const Held& held) {
  for (const std::uint32_t index : updated) {
    if (index >= count) {
      return OutOfRange(kind, index, count);
    }
    if (!IsHeld(held, index)) {
      return Inconsistent(
          std::format("{} {}: updated, and not held or removed", kind, index));
    }
  }
  return {};
}

// Holds the ids `added` in `held`, which must not hold them yet.
Checked Add(std::string_view kind, std::uint32_t count,
            std::span<const std::uint32_t> added, Held& held) {
  for (const std::uint32_t index : added) {
    if (index >= count) {
      return OutOfRange(kind, index, count);
    }
    if (IsHeld(held, index)) {
      return Inconsistent(
          std::format("{} {}: added, and already held", kind, index));
    }
    held.at(index) = 1;
  }
  return {};
}

// Checks that `held` holds no id not below `count`.
Checked CheckBelow(std::string_view kind, std::uint32_t count,
                   const Held& held) {
  for (std::size_t index = count; index < held.size(); ++index) {
    if (held.at(index) != 0) {
      return Inconsistent(std::format(
          "{} {}: still held, and its id is not below their count, {}", kind,
          index, count));
    }
  }
  return {};
}

// The ids of one kind held once `ids` are applied to `held`: its removals,
// then its additions. Fails when an id is not below `count`, an id is
// removed or updated that `held` does not hold, or added that it does, or
// an id not below `count` is still held once they are applied.
std::expected<Held, Error> ApplyIds(std::string_view kind, std::uint32_t count,
                                    const KindIds& ids, Held held) {
  if (held.size() < count) {
    held.resize(count, 0);
  }
  if (auto applied =
          Remove(kind, count, ids.removed, held)
              .and_then(
                  [&] { return CheckUpdated(kind, count, ids.updated, held); })
              .and_then([&] { return Add(kind, count, ids.added, held); })
              .and_then([&] { return CheckBelow(kind, count, held); });
      !applied.has_value()) {
    return std::unexpected(std::move(applied.error()));
  }
  held.resize(count);
  return held;
}

// What the primvars of a mesh are counted against.
struct MeshShape {
  std::size_t points = 0;
  std::size_t faces = 0;
  std::size_t triangles = 0;
};

// The values a primvar at `rate` needs on `shape`.
std::size_t Needed(PrimvarRate rate, const MeshShape& shape) {
  switch (rate) {
    case PrimvarRate::kConstant:
      return 1;
    case PrimvarRate::kPerFace:
      return shape.faces;
    case PrimvarRate::kPerVertex:
      return shape.points;
    case PrimvarRate::kPerCorner:
      return 0;
  }
  return 0;
}

// Checks the indices of a per-corner primvar of `count` values.
Checked CheckCorners(const Primvar& primvar, std::size_t count,
                     const MeshShape& shape, std::string_view owner) {
  if (primvar.indices.size() != shape.triangles * 3) {
    return Inconsistent(std::format(
        "{}: primvar {} has {} corner indices, for {} triangles", owner,
        primvar.name, primvar.indices.size(), shape.triangles));
  }
  const auto bad = std::ranges::find_if(
      primvar.indices, [count](std::uint32_t index) { return index >= count; });
  if (bad != primvar.indices.end()) {
    return Inconsistent(std::format("{}: primvar {} indexes value {}, of {}",
                                    owner, primvar.name, *bad, count));
  }
  return {};
}

// Checks the numbers of a primvar, named `what`: floats or integers, a
// whole number of values, aligned.
Checked CheckNumbers(const Primvar& primvar, std::string_view owner,
                     std::string_view what) {
  if (primvar.components == 0) {
    return Inconsistent(std::format("{}: {} has no components", owner, what));
  }
  if (!primvar.floats.empty() && !primvar.integers.empty()) {
    return Inconsistent(
        std::format("{}: {} has both floats and integers", owner, what));
  }
  const std::size_t numbers = primvar.floats.size() + primvar.integers.size();
  if (numbers % primvar.components != 0) {
    return Inconsistent(
        std::format("{}: {} has {} numbers, not a multiple of its {} "
                    "components",
                    owner, what, numbers, primvar.components));
  }
  return CheckAligned(primvar.floats, owner, std::format("{} floats", what))
      .and_then([&] {
        return CheckAligned(primvar.integers, owner,
                            std::format("{} integers", what));
      })
      .and_then([&] {
        return CheckAligned(primvar.indices, owner,
                            std::format("{} indices", what));
      });
}

// Checks that a primvar, named `what`, has as many values as its rate needs
// on `shape`.
Checked CheckCount(const Primvar& primvar, const MeshShape& shape,
                   std::string_view owner, std::string_view what) {
  const std::size_t count = primvar.Count();
  if (primvar.rate == PrimvarRate::kPerCorner) {
    return CheckCorners(primvar, count, shape, owner);
  }
  if (!primvar.indices.empty()) {
    return Inconsistent(
        std::format("{}: {} has indices, and is not per corner", owner, what));
  }
  const std::size_t needed = Needed(primvar.rate, shape);
  const bool matches = primvar.rate == PrimvarRate::kConstant ? count >= needed
                                                              : count == needed;
  if (!matches) {
    return Inconsistent(std::format("{}: {} has {} values, {} needed", owner,
                                    what, count, needed));
  }
  return {};
}

// Checks a primvar of a mesh of `shape`, or of an instance, which is
// constant.
Checked CheckPrimvar(const Primvar& primvar, const MeshShape& shape,
                     std::string_view owner) {
  const std::string what = std::format("primvar {}", primvar.name);
  return CheckNumbers(primvar, owner, what).and_then([&] {
    return CheckCount(primvar, shape, owner, what);
  });
}

// Checks each of `primvars`, of a mesh of `shape`.
Checked CheckPrimvars(std::span<const Primvar> primvars, const MeshShape& shape,
                      std::string_view owner) {
  for (const Primvar& primvar : primvars) {
    if (auto checked = CheckPrimvar(primvar, shape, owner);
        !checked.has_value()) {
      return checked;
    }
  }
  return {};
}

// Checks an instance, named `name`: a transform per motion sample, aligned,
// and constant primvars.
Checked CheckInstance(const Instance& instance, std::string_view name) {
  if (instance.object_to_world.empty()) {
    return Inconsistent(std::format("{}: no transform", name));
  }
  if (auto aligned = CheckAligned(instance.object_to_world, name, "transforms");
      !aligned.has_value()) {
    return aligned;
  }
  for (const Primvar& primvar : instance.primvars) {
    if (primvar.rate != PrimvarRate::kConstant) {
      return Inconsistent(
          std::format("{}: primvar {} is not constant", name, primvar.name));
    }
  }
  return CheckPrimvars(instance.primvars, {}, name);
}

// Checks the placements of `owner`: at least one, each valid.
Checked CheckInstances(std::span<const Instance> instances,
                       std::string_view owner) {
  if (instances.empty()) {
    return Inconsistent(std::format("{}: no instance", owner));
  }
  std::size_t index = 0;
  for (const Instance& instance : instances) {
    if (auto checked = CheckInstance(
            instance, std::format("{}, instance {}", owner, index++));
        !checked.has_value()) {
      return checked;
    }
  }
  return {};
}

// Checks the points of a prototype: one array or more, as long as each
// other, aligned.
Checked CheckPoints(const Prototype& prototype, std::string_view owner) {
  if (prototype.points.empty()) {
    return Inconsistent(std::format("{}: no points", owner));
  }
  const std::size_t count = prototype.points.front().size();
  for (const std::span<const Float3> sample : prototype.points) {
    if (sample.size() != count) {
      return Inconsistent(std::format("{}: motion samples of {} and {} points",
                                      owner, count, sample.size()));
    }
    if (auto aligned = CheckAligned(sample, owner, "points");
        !aligned.has_value()) {
      return aligned;
    }
  }
  return {};
}

// Checks the triangles of a prototype and their source faces.
Checked CheckTriangles(const Prototype& prototype, std::string_view owner) {
  const std::size_t points = prototype.points.front().size();
  for (const Triangle& triangle : prototype.triangles) {
    if (std::ranges::any_of(triangle, [points](std::uint32_t index) {
          return index >= points;
        })) {
      return Inconsistent(std::format(
          "{}: a triangle indexes a point not below {}", owner, points));
    }
  }
  if (prototype.source_faces.size() != prototype.triangles.size()) {
    return Inconsistent(std::format("{}: {} source faces, for {} triangles",
                                    owner, prototype.source_faces.size(),
                                    prototype.triangles.size()));
  }
  const std::size_t faces = prototype.face_vertex_counts.size();
  if (std::ranges::any_of(prototype.source_faces, [faces](std::uint32_t face) {
        return face >= faces;
      })) {
    return Inconsistent(
        std::format("{}: a source face not below {}", owner, faces));
  }
  return CheckAligned(prototype.triangles, owner, "triangles")
      .and_then([&] {
        return CheckAligned(prototype.source_faces, owner, "source faces");
      })
      .and_then([&] {
        return CheckAligned(prototype.face_vertex_counts, owner,
                            "face vertex counts");
      });
}

// Checks the materials of a prototype: one or more, each held, and those
// of its triangles.
Checked CheckMaterials(const Prototype& prototype, const Held& materials,
                       std::string_view owner) {
  if (prototype.materials.empty()) {
    return Inconsistent(std::format("{}: no material", owner));
  }
  for (const MaterialId material : prototype.materials) {
    if (!IsHeld(materials, std::to_underlying(material))) {
      return Inconsistent(std::format("{}: material {} is not held", owner,
                                      std::to_underlying(material)));
    }
  }
  const std::span<const std::uint32_t> triangles = prototype.triangle_materials;
  if (!triangles.empty() && triangles.size() != prototype.triangles.size()) {
    return Inconsistent(
        std::format("{}: {} triangle materials, for {} "
                    "triangles",
                    owner, triangles.size(), prototype.triangles.size()));
  }
  const std::size_t count = prototype.materials.size();
  if (std::ranges::any_of(
          triangles, [count](std::uint32_t index) { return index >= count; })) {
    return Inconsistent(
        std::format("{}: a triangle's material not below {}", owner, count));
  }
  return CheckAligned(prototype.materials, owner, "materials").and_then([&] {
    return CheckAligned(triangles, owner, "triangle materials");
  });
}

// Checks the normals of a prototype of `shape`: three floats at a rate but
// constant.
Checked CheckNormals(const Primvar& normals, const MeshShape& shape,
                     std::string_view owner) {
  if (normals.rate == PrimvarRate::kConstant || normals.components != 3 ||
      !normals.integers.empty()) {
    return Inconsistent(std::format(
        "{}: its normals are not three floats at a rate but constant", owner));
  }
  return CheckPrimvar(normals, shape, owner);
}

// Checks a prototype added or updated.
Checked CheckPrototype(const Prototype& prototype, const Held& materials) {
  const std::string owner = std::format(
      "prototype {} ({})", std::to_underlying(prototype.id), prototype.path);
  if (auto points = CheckPoints(prototype, owner); !points.has_value()) {
    return points;
  }
  const MeshShape shape{
      .points = prototype.points.front().size(),
      .faces = prototype.face_vertex_counts.size(),
      .triangles = prototype.triangles.size(),
  };
  return CheckTriangles(prototype, owner)
      .and_then([&] { return CheckNormals(prototype.normals, shape, owner); })
      .and_then([&] { return CheckPrimvars(prototype.primvars, shape, owner); })
      .and_then([&] { return CheckMaterials(prototype, materials, owner); })
      .and_then([&] { return CheckInstances(prototype.instances, owner); });
}

// Fails, naming `owner`, when `texture` is not held.
Checked CheckTextureHeld(TextureId texture, const Held& textures,
                         std::string_view owner) {
  if (!IsHeld(textures, std::to_underlying(texture))) {
    return Inconsistent(std::format("{}: texture {} is not held", owner,
                                    std::to_underlying(texture)));
  }
  return {};
}

// Checks a parameter of a node.
Checked CheckParameter(const Parameter& parameter, const Held& textures,
                       std::string_view owner) {
  const std::string what = std::format("parameter {}", parameter.name);
  return std::visit(
      [&]<typename T>(const T& value) -> Checked {
        if constexpr (std::is_same_v<T, TextureId>) {
          return CheckTextureHeld(value, textures, owner);
        } else if constexpr (std::is_same_v<T, std::span<const float>> ||
                             std::is_same_v<T, std::span<const std::int32_t>>) {
          return CheckAligned(value, owner, what);
        } else {
          return {};
        }
      },
      parameter.value);
}

// Checks a node, named `name`, of a material of `nodes` nodes: its inputs
// come from nodes of the material, and its textures are held.
Checked CheckNode(const MaterialNode& node, std::size_t nodes,
                  const Held& textures, std::string_view name) {
  for (const Connection& connection : node.connections) {
    if (connection.node >= nodes) {
      return Inconsistent(std::format("{}: input {} comes from node {}, of {}",
                                      name, connection.input, connection.node,
                                      nodes));
    }
  }
  for (const Parameter& parameter : node.parameters) {
    if (auto checked = CheckParameter(parameter, textures, name);
        !checked.has_value()) {
      return checked;
    }
  }
  return {};
}

// Checks a material added or updated.
Checked CheckMaterial(const MaterialNetwork& material, const Held& textures) {
  const std::string owner = std::format(
      "material {} ({})", std::to_underlying(material.id), material.path);
  const std::size_t nodes = material.nodes.size();
  if (material.surface.node >= nodes) {
    return Inconsistent(std::format("{}: its surface is node {}, of {}", owner,
                                    material.surface.node, nodes));
  }
  for (const MaterialNode& node : material.nodes) {
    if (auto checked = CheckNode(node, nodes, textures,
                                 std::format("{}, node {}", owner, node.name));
        !checked.has_value()) {
      return checked;
    }
  }
  return {};
}

// The texture of a light's shape, if any.
std::optional<TextureId> TextureOf(const Light::Shape& shape) {
  if (const auto* rect = std::get_if<Light::Rect>(&shape)) {
    return rect->texture;
  }
  if (const auto* dome = std::get_if<Light::Dome>(&shape)) {
    return dome->texture;
  }
  return std::nullopt;
}

// Checks a light added or updated.
Checked CheckLight(const Light& light, const Held& textures) {
  const std::string owner =
      std::format("light {} ({})", std::to_underlying(light.id), light.path);
  if (const std::optional<TextureId> texture = TextureOf(light.shape)) {
    if (auto held = CheckTextureHeld(*texture, textures, owner);
        !held.has_value()) {
      return held;
    }
  }
  for (const Instance& instance : light.instances) {
    if (!instance.primvars.empty()) {
      return Inconsistent(std::format("{}: an instance has primvars", owner));
    }
  }
  return CheckInstances(light.instances, owner);
}

// Checks a camera added or updated.
Checked CheckCamera(const Camera& camera) {
  const std::string owner =
      std::format("camera {} ({})", std::to_underlying(camera.id), camera.path);
  if (camera.camera_to_world.empty()) {
    return Inconsistent(std::format("{}: no transform", owner));
  }
  return CheckAligned(camera.camera_to_world, owner, "transforms");
}

// Checks a texture added or updated: an absolute file, with a color space.
Checked CheckTexture(const Texture& texture) {
  const std::string owner = std::format(
      "texture {} ({})", std::to_underlying(texture.id), texture.file);
  if (!texture.file.starts_with('/')) {
    return Inconsistent(std::format("{}: its file is not absolute", owner));
  }
  if (texture.color_space.empty()) {
    return Inconsistent(std::format("{}: no color space", owner));
  }
  return {};
}

// Checks each of `views`, added or updated, with `check`.
template <typename View, typename Check>
Checked CheckEach(std::span<const View> views, const Check& check) {
  for (const View& view : views) {
    if (auto checked = check(view); !checked.has_value()) {
      return checked;
    }
  }
  return {};
}

// Checks the content of the views of `changes`, added and updated, with
// `check`.
template <typename View, typename Id, typename Check>
Checked CheckViews(const Changes<View, Id>& changes, const Check& check) {
  return CheckEach(changes.added, check).and_then([&] {
    return CheckEach(changes.updated, check);
  });
}

// What a commit holds of every kind, once applied.
struct Tables {
  Held prototypes;
  Held lights;
  Held materials;
  Held cameras;
  Held textures;
};

// Applies the ids of one kind to `held`, which keeps what it held when they
// do not apply.
Checked Apply(std::string_view kind, std::uint32_t count, const KindIds& ids,
              Held& held) {
  auto applied = ApplyIds(kind, count, ids, held);
  if (!applied.has_value()) {
    return std::unexpected(std::move(applied.error()));
  }
  held = std::move(*applied);
  return {};
}

// Applies the ids of `changes` to `tables`.
Checked ApplyAll(const ChangeSet& changes, Tables& tables) {
  return Apply("prototype", changes.ids.prototypes, IdsOf(changes.prototypes),
               tables.prototypes)
      .and_then([&] {
        return Apply("light", changes.ids.lights, IdsOf(changes.lights),
                     tables.lights);
      })
      .and_then([&] {
        return Apply("material", changes.ids.materials,
                     IdsOf(changes.materials), tables.materials);
      })
      .and_then([&] {
        return Apply("camera", changes.ids.cameras, IdsOf(changes.cameras),
                     tables.cameras);
      })
      .and_then([&] {
        return Apply("texture", changes.ids.textures, IdsOf(changes.textures),
                     tables.textures);
      });
}

// Checks the content of every view of `changes` against `tables`, the ids
// held once they are applied.
Checked CheckAll(const ChangeSet& changes, const Tables& tables) {
  return CheckViews(changes.textures, CheckTexture)
      .and_then([&] { return CheckViews(changes.cameras, CheckCamera); })
      .and_then([&] {
        return CheckViews(changes.materials,
                          [&](const MaterialNetwork& material) {
                            return CheckMaterial(material, tables.textures);
                          });
      })
      .and_then([&] {
        return CheckViews(changes.lights, [&](const Light& light) {
          return CheckLight(light, tables.textures);
        });
      })
      .and_then([&] {
        return CheckEach(changes.prototypes.added,
                         [&](const Prototype& prototype) {
                           return CheckPrototype(prototype, tables.materials);
                         });
      })
      .and_then([&] {
        return CheckEach(
            changes.prototypes.updated, [&](const UpdatedPrototype& updated) {
              return CheckPrototype(updated.prototype, tables.materials);
            });
      });
}

// "1 camera", "12 prototypes".
std::string Counted(std::size_t count, std::string_view noun) {
  return std::format("{} {}{}", core::FormatCount(count), noun,
                     count == 1 ? "" : "s");
}

// "12 prototypes (2.1M instances, 1.24M triangles), 14 lights, 57
// materials, 1 camera, 210 textures".
std::string DescribeAdded(const ChangeSet& changes) {
  std::size_t instances = 0;
  std::size_t triangles = 0;
  for (const Prototype& prototype : changes.prototypes.added) {
    instances += prototype.instances.size();
    triangles += prototype.triangles.size();
  }
  return std::format("{} ({}, {}), {}, {}, {}, {}",
                     Counted(changes.prototypes.added.size(), "prototype"),
                     Counted(instances, "instance"),
                     Counted(triangles, "triangle"),
                     Counted(changes.lights.added.size(), "light"),
                     Counted(changes.materials.added.size(), "material"),
                     Counted(changes.cameras.added.size(), "camera"),
                     Counted(changes.textures.added.size(), "texture"));
}

// The number of prims `changes` updates, or removes.
std::size_t CountUpdated(const ChangeSet& changes) {
  return changes.prototypes.updated.size() + changes.lights.updated.size() +
         changes.materials.updated.size() + changes.cameras.updated.size() +
         changes.textures.updated.size();
}

std::size_t CountRemoved(const ChangeSet& changes) {
  return changes.prototypes.removed.size() + changes.lights.removed.size() +
         changes.materials.removed.size() + changes.cameras.removed.size() +
         changes.textures.removed.size();
}

// Traces each prim `changes` adds.
void TraceAdded(const ChangeSet& changes) {
  for (const Prototype& prototype : changes.prototypes.added) {
    UMBRA_LOG_TRACE(kDevice, "added prototype {} ({})",
                    std::to_underlying(prototype.id), prototype.path);
  }
  for (const Light& light : changes.lights.added) {
    UMBRA_LOG_TRACE(kDevice, "added light {} ({})",
                    std::to_underlying(light.id), light.path);
  }
  for (const MaterialNetwork& material : changes.materials.added) {
    UMBRA_LOG_TRACE(kDevice, "added material {} ({})",
                    std::to_underlying(material.id), material.path);
  }
  for (const Camera& camera : changes.cameras.added) {
    UMBRA_LOG_TRACE(kDevice, "added camera {} ({})",
                    std::to_underlying(camera.id), camera.path);
  }
  for (const Texture& texture : changes.textures.added) {
    UMBRA_LOG_TRACE(kDevice, "added texture {} ({}, {})",
                    std::to_underlying(texture.id), texture.file,
                    texture.color_space);
  }
}

}  // namespace

std::string_view DiagnosticDevice::name() const { return "diagnostic"; }

std::expected<void, Error> DiagnosticDevice::Commit(const ChangeSet& changes) {
  ++commits_;
  UMBRA_LOG_DEBUG(kDevice,
                  "diagnostic: commit {}: {} added, {} updated, {} "
                  "removed",
                  commits_, DescribeAdded(changes), CountUpdated(changes),
                  CountRemoved(changes));
  TraceAdded(changes);
  Tables tables{
      .prototypes = prototypes_,
      .lights = lights_,
      .materials = materials_,
      .cameras = cameras_,
      .textures = textures_,
  };
  if (auto checked = ApplyAll(changes, tables).and_then([&] {
        return CheckAll(changes, tables);
      });
      !checked.has_value()) {
    return checked;
  }
  prototypes_ = std::move(tables.prototypes);
  lights_ = std::move(tables.lights);
  materials_ = std::move(tables.materials);
  cameras_ = std::move(tables.cameras);
  textures_ = std::move(tables.textures);
  UMBRA_LOG_DEBUG(kDevice, "diagnostic: commit {} is consistent", commits_);
  return {};
}

}  // namespace umbra::device
