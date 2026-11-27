// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "commit_description.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <functional>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ledger.h"
#include "plural.h"
#include "umbra/core/number_format.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/device/render_data.h"

namespace umbra::delegate::internal {
namespace {

// "render/beauty.0012.exr [color, depth]".
std::string DescribeProduct(const RenderProduct& product) {
  std::string vars;
  for (const RenderVar& var : product.vars) {
    vars += vars.empty() ? var.name : ", " + var.name;
  }
  return std::format("{} [{}]", product.file, vars);
}

// "1.24M triangles in 3.4k meshes, 18 primvars, 2.1M instances of 3.4k
// prototypes": the primvars counted by name, the normals apart.
std::string DescribeGeometry(const device::ChangeSet& changes) {
  std::size_t triangles = 0;
  std::size_t instances = 0;
  std::set<std::string_view> primvars;
  for (const device::Prototype& prototype : changes.prototypes.added) {
    triangles += prototype.triangles.size();
    instances += prototype.instances.size();
    for (const device::Primvar& primvar : prototype.primvars) {
      primvars.insert(primvar.name);
    }
  }
  const std::size_t meshes = changes.prototypes.added.size();
  return std::format(
      "{} {} in {} {}, {} {}, {} {} of {} {}", core::FormatCount(triangles),
      Plural(triangles, "triangle", "triangles"), core::FormatCount(meshes),
      Plural(meshes, "mesh", "meshes"), core::FormatCount(primvars.size()),
      Plural(primvars.size(), "primvar", "primvars"),
      core::FormatCount(instances), Plural(instances, "instance", "instances"),
      core::FormatCount(meshes), Plural(meshes, "prototype", "prototypes"));
}

// The kinds of material the debug line counts.
struct MaterialKinds {
  std::size_t materialx = 0;
  std::size_t preview_surface = 0;
  std::size_t osl = 0;
};

// Counts `material` into `kinds`: OSL when a node is an .oso shader,
// UsdPreviewSurface when its surface is, MaterialX otherwise.
void CountKind(const device::MaterialNetwork& material, MaterialKinds& kinds) {
  if (std::ranges::any_of(material.nodes, [](const device::MaterialNode& node) {
        return node.kind == device::ShaderKind::kOsl;
      })) {
    ++kinds.osl;
  } else if (material.nodes.subspan(material.surface.node).front().shader ==
             "ND_UsdPreviewSurface_surfaceshader") {
    ++kinds.preview_surface;
  } else {
    ++kinds.materialx;
  }
}

// "MaterialX 40, UsdPreviewSurface 15, OSL 2": the kinds counted, but those
// of none.
std::string DescribeKinds(const MaterialKinds& kinds) {
  const auto named = std::to_array<std::pair<std::string_view, std::size_t>>({
      {"MaterialX", kinds.materialx},
      {"UsdPreviewSurface", kinds.preview_surface},
      {"OSL", kinds.osl},
  });
  std::string counts;
  for (const auto& [name, count] : named) {
    if (count > 0) {
      counts += std::format("{}{} {}", counts.empty() ? "" : ", ", name,
                            core::FormatCount(count));
    }
  }
  return counts;
}

// The prototypes added whose own material is `display_color`, the display
// color material.
std::size_t CountOnDisplayColor(
    const device::ChangeSet& changes,
    std::optional<device::MaterialId> display_color) {
  return static_cast<std::size_t>(std::ranges::count_if(
      changes.prototypes.added, [&](const device::Prototype& prototype) {
        return !prototype.materials.empty() &&
               prototype.materials.front() == display_color;
      }));
}

// Inserts into `read` the ids of the textures `node` reads.
void InsertTextureIds(const device::MaterialNode& node,
                      std::set<device::TextureId>& read) {
  for (const device::Parameter& parameter : node.parameters) {
    if (const auto* texture =
            std::get_if<device::TextureId>(&parameter.value)) {
      read.insert(*texture);
    }
  }
}

// The files of the textures the materials `changes` adds read, by their
// ids in `textures`.
std::set<std::string_view> MaterialTextures(const device::ChangeSet& changes,
                                            const HeldTextures& textures) {
  std::set<device::TextureId> read;
  for (const device::MaterialNetwork& material : changes.materials.added) {
    for (const device::MaterialNode& node : material.nodes) {
      InsertTextureIds(node, read);
    }
  }
  std::set<std::string_view> files;
  for (const auto& [texture, held] : textures) {
    if (read.contains(held.id)) {
      files.insert(texture.file);
    }
  }
  return files;
}

// "57 materials (MaterialX 40, UsdPreviewSurface 15, OSL 2), 210 textures,
// 300 meshes on displayColor": those added, the textures counted by file.
std::string DescribeMaterials(const device::ChangeSet& changes,
                              const HeldTextures& textures,
                              std::optional<device::MaterialId> display_color) {
  MaterialKinds kinds;
  for (const device::MaterialNetwork& material : changes.materials.added) {
    CountKind(material, kinds);
  }
  const std::string counts = DescribeKinds(kinds);
  const std::size_t texture_count = MaterialTextures(changes, textures).size();
  const std::size_t materials = changes.materials.added.size();
  const std::size_t on_display_color =
      CountOnDisplayColor(changes, display_color);
  return std::format("{} {}{}, {} {}, {} {} on displayColor",
                     core::FormatCount(materials),
                     Plural(materials, "material", "materials"),
                     counts.empty() ? "" : std::format(" ({})", counts),
                     core::FormatCount(texture_count),
                     Plural(texture_count, "texture", "textures"),
                     core::FormatCount(on_display_color),
                     Plural(on_display_color, "mesh", "meshes"));
}

// "14 lights (rect 8, sphere 4, distant 1, dome 1)": the lights counted by
// kind, one for each instance, the most first.
std::string DescribeLights(const device::ChangeSet& changes) {
  constexpr auto kKinds = device::kLightShapeNames;
  std::array<std::size_t, kKinds.size()> counts{};
  std::size_t lights = 0;
  for (const device::Light& light : changes.lights.added) {
    counts.at(light.shape.index()) += light.instances.size();
    lights += light.instances.size();
  }
  std::array<std::size_t, kKinds.size()> order{};
  std::ranges::iota(order, std::size_t{0});
  std::ranges::stable_sort(
      order, std::ranges::greater{},
      [&counts](std::size_t kind) { return counts.at(kind); });
  std::string kinds;
  for (const std::size_t kind : order) {
    if (counts.at(kind) > 0) {
      kinds += std::format("{}{} {}", kinds.empty() ? "" : ", ",
                           kKinds.at(kind), core::FormatCount(counts.at(kind)));
    }
  }
  return std::format("{} {}{}", core::FormatCount(lights),
                     Plural(lights, "light", "lights"),
                     kinds.empty() ? "" : std::format(" ({})", kinds));
}

// The motion of what the debug line counts: how many move, and the most
// motion samples of any.
struct MotionCount {
  std::size_t moving = 0;
  std::size_t samples = 1;

  void Add(std::size_t count) {
    moving += count > 1 ? 1 : 0;
    samples = std::max(samples, count);
  }
};

// "shutter -0.25..0.25, 1.2k moving (max 4 samples)": the prototypes whose
// points move and the instances that move, of prototypes and of lights, and
// the most motion samples of any of them or of a camera.
std::string DescribeMotion(const device::ChangeSet& changes,
                           const device::Shutter& shutter) {
  MotionCount motion;
  for (const device::Prototype& prototype : changes.prototypes.added) {
    motion.Add(prototype.points.size());
    for (const device::Instance& instance : prototype.instances) {
      motion.Add(instance.object_to_world.size());
    }
  }
  for (const device::Light& light : changes.lights.added) {
    for (const device::Instance& instance : light.instances) {
      motion.Add(instance.object_to_world.size());
    }
  }
  for (const device::Camera& camera : changes.cameras.added) {
    motion.samples = std::max(motion.samples, camera.camera_to_world.size());
  }
  return std::format("shutter {}..{}, {} moving (max {} {})", shutter.open,
                     shutter.close, core::FormatCount(motion.moving),
                     motion.samples,
                     Plural(motion.samples, "sample", "samples"));
}

// The number of prims `changes` updates, or removes.
std::size_t CountUpdated(const device::ChangeSet& changes) {
  return changes.prototypes.updated.size() + changes.lights.updated.size() +
         changes.materials.updated.size() + changes.cameras.updated.size();
}

std::size_t CountRemoved(const device::ChangeSet& changes) {
  return changes.prototypes.removed.size() + changes.lights.removed.size() +
         changes.materials.removed.size() + changes.cameras.removed.size() +
         changes.textures.removed.size();
}

}  // namespace

std::string Describe(const RenderSettings& settings,
                     const device::ChangeSet& changes,
                     const device::Shutter& shutter,
                     const Description& description) {
  std::string products;
  for (const RenderProduct& product : settings.products) {
    products += (products.empty() ? "" : ", ") + DescribeProduct(product);
  }
  const std::size_t count = settings.products.size();
  const std::size_t updated = CountUpdated(changes);
  return std::format(
      "settings {}, camera {}, {} {}: {}, {}, {}, {}, {}, {} {} updated, {} "
      "removed",
      settings.path, description.camera, count,
      Plural(count, "product", "products"), products, DescribeGeometry(changes),
      DescribeMaterials(changes, *description.textures,
                        description.display_color),
      DescribeLights(changes), DescribeMotion(changes, shutter),
      core::FormatCount(updated), Plural(updated, "prim", "prims"),
      core::FormatCount(CountRemoved(changes)));
}

}  // namespace umbra::delegate::internal
