// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "commit.h"

#include <pxr/base/gf/range2f.h>
#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/renderSettings.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/sceneIndexPrimView.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdRender/tokens.h>
#include <pxr/usd/usdRender/var.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "host_copy.h"
#include "hydra.h"
#include "light.h"
#include "material.h"
#include "motion.h"
#include "outputs.h"
#include "umbra/core/error.h"
#include "umbra/core/floating_point.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/device/render_data.h"
#include "umbra/ext/usd/render_settings.h"
#include "views.h"

namespace umbra::ext::usd::internal {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;
using Product = pxr::HdRenderSettings::RenderProduct;
using Var = Product::RenderVar;

// The warnings of the commits, each logged once: a commit that finds the
// same prims at fault as an earlier one does not log them again.
class Warnings {
 public:
  explicit Warnings(std::set<std::string>& logged UMBRA_LIFETIMEBOUND)
      : logged_(&logged) {}

  void Warn(std::string message) {
    if (logged_->insert(message).second) {
      UMBRA_LOG_WARNING(kUsd, "{}", message);
    }
  }

 private:
  std::set<std::string>* logged_;
};

// The prims of one kind a warning reports: their count and the first.
class Tally {
 public:
  void Add(const pxr::SdfPath& path, std::string_view detail = {}) {
    if (count_ == 0) {
      first_ = path.GetString();
      detail_ = detail;
    }
    ++count_;
  }

  [[nodiscard]] std::size_t count() const { return count_; }
  // "/World/Car/Body" for one, "first /World/Car/Body" for more.
  [[nodiscard]] std::string First() const {
    return count_ == 1 ? first_ : "first " + first_;
  }
  [[nodiscard]] const std::string& detail() const UMBRA_LIFETIMEBOUND {
    return detail_;
  }

 private:
  std::size_t count_ = 0;
  std::string first_;
  std::string detail_;
};

// `singular` for one, `plural` otherwise.
std::string_view Plural(std::size_t count, std::string_view singular,
                        std::string_view plural) {
  return count == 1 ? singular : plural;
}

// The prims whose motion sampling changed, for the warnings that report
// them.
class MotionTallies {
 public:
  void Add(const pxr::SdfPath& path, MotionOutcome outcome) {
    switch (outcome) {
      case MotionOutcome::kSampled:
        break;
      case MotionOutcome::kResampledDown:
        resampled_.Add(path);
        break;
      case MotionOutcome::kTopologyChanges:
        changing_.Add(path);
        break;
    }
  }

  // "warning: usd: 3 prims change topology within the shutter, rendered
  // without motion blur (first /World/fluid)", and "warning: usd: 2 prims
  // have more than 16 motion samples within the shutter, resampled down to
  // 16 (first /World/sim)".
  void Report(Warnings& warnings) const {
    if (changing_.count() > 0) {
      warnings.Warn(std::format(
          "{} {} topology within the shutter, rendered without motion blur "
          "({})",
          changing_.count(),
          Plural(changing_.count(), "prim changes", "prims change"),
          changing_.First()));
    }
    if (resampled_.count() > 0) {
      warnings.Warn(std::format(
          "{} {} more than {} motion samples within the "
          "shutter, resampled down to {} ({})",
          resampled_.count(),
          Plural(resampled_.count(), "prim has", "prims have"),
          kMaxMotionSamples, kMaxMotionSamples, resampled_.First()));
    }
  }

 private:
  Tally changing_;
  Tally resampled_;
};

// Whether the shutter changed since the last commit: every camera is then
// placed again.
enum class ShutterChange : std::uint8_t {
  kSame,
  kChanged,
};

// The cameras the settings and their products see through: each added the
// first commit it is, updated when it synced or the shutter changed, and
// removed once it is not.
class Cameras {
 public:
  Cameras(const SyncedPrims& prims UMBRA_LIFETIMEBOUND,
          Ledger& ledger UMBRA_LIFETIMEBOUND,
          MotionTallies& motion UMBRA_LIFETIMEBOUND,
          ChangeSetBuilder& changes UMBRA_LIFETIMEBOUND, ShutterChange shutter)
      : prims_(&prims),
        ledger_(&ledger),
        motion_(&motion),
        changes_(&changes),
        shutter_(shutter) {}

  // The id of the camera at `path`, which `owner` names: fails when there
  // is none, or it is not a Camera.
  std::expected<device::CameraId, Error> Index(const pxr::SdfPath& path,
                                               std::string_view owner);

  // Removes the cameras held that no longer are seen through.
  void RemoveUnseen();

 private:
  const SyncedPrims* prims_;
  Ledger* ledger_;
  MotionTallies* motion_;
  ChangeSetBuilder* changes_;
  ShutterChange shutter_;
  // The cameras seen through by this commit.
  std::map<pxr::SdfPath, device::CameraId> seen_;
};

std::expected<device::CameraId, Error> Cameras::Index(const pxr::SdfPath& path,
                                                      std::string_view owner) {
  if (path.IsEmpty()) {
    return std::unexpected(Error(ErrorCode::kInvalidArgument,
                                 std::format("{} have no camera", owner)));
  }
  if (const auto known = seen_.find(path); known != seen_.end()) {
    return known->second;
  }
  const auto synced = prims_->cameras.find(path);
  if (synced == prims_->cameras.end()) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("{}: the camera {} is not a Camera", owner,
                          path.GetString())));
  }
  CameraPrim& camera = *synced->second;
  const auto held = ledger_->cameras.find(path);
  device::CameraId camera_id{};
  if (held == ledger_->cameras.end()) {
    camera.Place(prims_->shutter);
    camera_id = ledger_->camera_ids.Take();
    ledger_->cameras.emplace(path, camera_id);
    changes_->AddCamera(camera_id, camera.camera());
  } else {
    camera_id = held->second;
    if (camera.IsChanged() || shutter_ == ShutterChange::kChanged) {
      camera.Place(prims_->shutter);
      changes_->UpdateCamera(camera_id, camera.camera());
    }
  }
  motion_->Add(path, camera.motion_outcome());
  seen_.emplace(path, camera_id);
  return camera_id;
}

void Cameras::RemoveUnseen() {
  std::erase_if(ledger_->cameras, [this](const auto& held) {
    if (seen_.contains(held.first)) {
      return false;
    }
    changes_->RemoveCamera(held.second);
    ledger_->camera_ids.Free(held.second);
    return true;
  });
}

// The render vars left out, for the one warning that reports them.
class UnsupportedVars {
 public:
  void Add(std::string_view name, const pxr::SdfPath& product) {
    if (count_ == 0) {
      first_name_ = name;
      first_product_ = product.GetString();
    }
    ++count_;
  }

  // "warning: usd: 3 render vars not supported (primId, first in
  // /Render/beauty); left out".
  void Report(Warnings& warnings) const {
    if (count_ == 1) {
      warnings.Warn(
          std::format("1 render var not supported ({}, in {}); left out",
                      first_name_, first_product_));
    } else if (count_ > 1) {
      warnings.Warn(
          std::format("{} render vars not supported ({}, first in {}); left "
                      "out",
                      count_, first_name_, first_product_));
    }
  }

 private:
  std::size_t count_ = 0;
  std::string first_name_;
  std::string first_product_;
};

// The materials, the textures and the terminals the warnings about
// materials report.
class MaterialTallies {
 public:
  void Add(const pxr::SdfPath& path, const TranslatedMaterial& translation) {
    if (!translation.network.has_value()) {
      untranslated_.Add(path, translation.network.error());
    }
    for (const UnresolvedTexture& texture : translation.unresolved) {
      unresolved_.Add(texture.node, texture.asset);
    }
    for (const std::string& terminal : translation.unsupported_terminals) {
      terminals_[terminal].Add(path);
    }
  }

  void Report(Warnings& warnings) const {
    ReportUntranslated(warnings);
    ReportUnresolved(warnings);
    ReportTerminals(warnings);
  }

 private:
  void ReportUntranslated(Warnings& warnings) const;
  void ReportUnresolved(Warnings& warnings) const;
  void ReportTerminals(Warnings& warnings) const;

  Tally untranslated_;
  Tally unresolved_;
  std::map<std::string, Tally> terminals_;
};

void MaterialTallies::ReportUntranslated(Warnings& warnings) const {
  if (untranslated_.count() > 0) {
    warnings.Warn(std::format(
        "{} {} not translate, rendered with the fallback material ({}: "
        "{})",
        untranslated_.count(),
        Plural(untranslated_.count(), "material does", "materials do"),
        untranslated_.First(), untranslated_.detail()));
  }
}

void MaterialTallies::ReportUnresolved(Warnings& warnings) const {
  if (unresolved_.count() > 0) {
    warnings.Warn(
        std::format("{} {} not resolve, rendered with {} node's fallback value "
                    "({}: {})",
                    unresolved_.count(),
                    Plural(unresolved_.count(), "texture does", "textures do"),
                    Plural(unresolved_.count(), "its", "their"),
                    unresolved_.First(), unresolved_.detail()));
  }
}

void MaterialTallies::ReportTerminals(Warnings& warnings) const {
  for (const auto& [terminal, tally] : terminals_) {
    warnings.Warn(std::format(
        "{} {} a {}, which umbra does not render yet; {} "
        "surface is rendered ({})",
        tally.count(),
        Plural(tally.count(), "material connects", "materials connect"),
        terminal, Plural(tally.count(), "its", "their"), tally.First()));
  }
}

// The textures the materials and the lights read: each added when the
// first reads it, and removed when the last stops.
class Textures {
 public:
  Textures(Ledger& ledger UMBRA_LIFETIMEBOUND,
           ChangeSetBuilder& changes UMBRA_LIFETIMEBOUND)
      : ledger_(&ledger), changes_(&changes) {}

  // Counts a reader of each of `textures`.
  void Read(const std::vector<host::Texture>& textures);
  // Counts a reader less of each of `textures`.
  void Unread(const std::vector<host::Texture>& textures);

 private:
  Ledger* ledger_;
  ChangeSetBuilder* changes_;
};

void Textures::Read(const std::vector<host::Texture>& textures) {
  for (const host::Texture& texture : textures) {
    if (++ledger_->texture_readers[texture] > 1) {
      continue;
    }
    const device::TextureId texture_id = ledger_->texture_ids.Take();
    // The change set views the ledger's copy, which outlives the commit.
    const host::Texture& held =
        ledger_->textures.emplace(texture, texture_id).first->first;
    changes_->AddTexture(texture_id, held);
  }
}

void Textures::Unread(const std::vector<host::Texture>& textures) {
  for (const host::Texture& texture : textures) {
    const auto readers = ledger_->texture_readers.find(texture);
    if (readers == ledger_->texture_readers.end() || --readers->second > 0) {
      continue;
    }
    ledger_->texture_readers.erase(readers);
    const auto held = ledger_->textures.find(texture);
    changes_->RemoveTexture(held->second);
    ledger_->texture_ids.Free(held->second);
    ledger_->textures.erase(held);
  }
}

// The textures the parameters of `node` read, in order.
auto TexturesOf(const host::MaterialNode& node) {
  return node.parameters |
         std::views::transform([](const host::Parameter& parameter) {
           return std::get_if<host::Texture>(&parameter.value);
         }) |
         std::views::filter(
             [](const host::Texture* texture) { return texture != nullptr; }) |
         std::views::transform(
             [](const host::Texture* texture) { return *texture; });
}

// The textures the parameters of `network` read, in order.
std::vector<host::Texture> TexturesOf(const host::MaterialNetwork& network) {
  return std::ranges::to<std::vector>(
      network.nodes | std::views::transform([](const host::MaterialNode& node) {
        return TexturesOf(node);
      }) |
      std::views::join);
}

// The texture of `light`'s shape, if any.
std::vector<host::Texture> TexturesOf(const host::Light& light) {
  std::optional<host::Texture> texture;
  if (const auto* rect = std::get_if<host::Light::Rect>(&light.shape)) {
    texture = rect->texture;
  } else if (const auto* dome = std::get_if<host::Light::Dome>(&light.shape)) {
    texture = dome->texture;
  }
  if (!texture.has_value()) {
    return {};
  }
  return {std::move(*texture)};
}

// The materials: every material prim, from its first sync to its
// destruction, and umbra's two while a prototype reads them. A material
// prim a prototype still reads outlives its prim until none does.
class Materials {
 public:
  Materials(const SyncedPrims& prims UMBRA_LIFETIMEBOUND,
            Ledger& ledger UMBRA_LIFETIMEBOUND,
            ChangeSetBuilder& changes UMBRA_LIFETIMEBOUND,
            Textures& textures UMBRA_LIFETIMEBOUND)
      : prims_(&prims),
        ledger_(&ledger),
        changes_(&changes),
        textures_(&textures) {}
  Materials(const Materials&) = delete;
  Materials& operator=(const Materials&) = delete;
  Materials(Materials&&) = delete;
  Materials& operator=(Materials&&) = delete;
  ~Materials() = default;

  // Adds or updates the material prims that synced since the last commit:
  // their networks when they translate, the fallback material otherwise.
  void CommitPrims();

  // The key of the material bound to a mesh, or to one of its GeomSubsets,
  // at `material`: its path when a material prim is there; else, the
  // display color material when the mesh has a displayColor, and the
  // fallback material otherwise.
  [[nodiscard]] std::string KeyOf(const pxr::SdfPath& material,
                                  DisplayColor display_color) const;

  // Counts a prototype reading each material of `keys`, adding umbra's
  // when first read, and gives their ids.
  std::vector<device::MaterialId> Read(const std::vector<std::string>& keys);
  // Counts a prototype less reading each material of `keys`.
  void Unread(const std::vector<std::string>& keys);

  // Removes the materials whose prim is gone, and umbra's, once no
  // prototype reads them.
  void RemoveUnread();

  // The id of the display color material, when it is held.
  [[nodiscard]] std::optional<device::MaterialId> DisplayColorId() const;

  // The error that stops the render: the first of the material prims
  // committed.
  [[nodiscard]] const std::optional<Error>& fatal() const UMBRA_LIFETIMEBOUND {
    return fatal_;
  }

  // "warning: usd: 2 materials do not translate, rendered with the fallback
  // material (first /World/Looks/Skin: ...)", "warning: usd: 3 textures do
  // not resolve, rendered with their node's fallback value (first
  // /World/Looks/Car/Albedo: ./tex/a.exr)", and one per terminal umbra does
  // not take.
  void Report(Warnings& warnings) const;

 private:
  // Adds or updates the material of `key`, as `network`, which must
  // outlive the commit.
  void Commit(const std::string& key, const host::MaterialNetwork& network);
  // Adds or updates the material prim `prim`, at `path`.
  void CommitPrim(const pxr::SdfPath& path, const MaterialPrim& prim);

  const SyncedPrims* prims_;
  Ledger* ledger_;
  ChangeSetBuilder* changes_;
  Textures* textures_;
  // The networks the commit makes: umbra's, and the fallback material of
  // each material prim that does not translate. A deque keeps them in
  // place.
  std::deque<host::MaterialNetwork> made_;
  std::optional<Error> fatal_;
};

void Materials::Commit(const std::string& key,
                       const host::MaterialNetwork& network) {
  std::vector<host::Texture> read = TexturesOf(network);
  textures_->Read(read);
  const auto [held, added] = ledger_->materials.try_emplace(key);
  if (added) {
    held->second.id = ledger_->material_ids.Take();
    changes_->AddMaterial(held->second.id, network);
  } else {
    changes_->UpdateMaterial(held->second.id, network);
  }
  textures_->Unread(std::exchange(held->second.textures, std::move(read)));
}

void Materials::CommitPrim(const pxr::SdfPath& path, const MaterialPrim& prim) {
  if (!prim.translation().has_value()) {
    if (!fatal_.has_value()) {
      fatal_ = prim.translation().error();
    }
    return;
  }
  const host::MaterialNetwork* network = prim.network();
  if (network == nullptr) {
    host::MaterialNetwork& fallback = made_.emplace_back(FallbackMaterial());
    fallback.path = path.GetString();
    network = &fallback;
  }
  Commit(path.GetString(), *network);
}

void Materials::CommitPrims() {
  for (const auto& [path, prim] : prims_->materials) {
    if (prim->IsChanged()) {
      CommitPrim(path, *prim);
    }
  }
}

std::string Materials::KeyOf(const pxr::SdfPath& material,
                             DisplayColor display_color) const {
  if (prims_->materials.contains(material)) {
    return material.GetString();
  }
  return std::string(display_color == DisplayColor::kPresent
                         ? kDisplayColorMaterial
                         : kFallbackMaterial);
}

std::vector<device::MaterialId> Materials::Read(
    const std::vector<std::string>& keys) {
  std::vector<device::MaterialId> ids;
  ids.reserve(keys.size());
  for (const std::string& key : keys) {
    if (!ledger_->materials.contains(key)) {
      Commit(key, made_.emplace_back(key == kDisplayColorMaterial
                                         ? DisplayColorMaterial()
                                         : FallbackMaterial()));
    }
    HeldMaterial& held = ledger_->materials.at(key);
    ++held.readers;
    ids.push_back(held.id);
  }
  return ids;
}

void Materials::Unread(const std::vector<std::string>& keys) {
  for (const std::string& key : keys) {
    --ledger_->materials.at(key).readers;
  }
}

void Materials::RemoveUnread() {
  std::erase_if(ledger_->materials, [this](const auto& entry) {
    const auto& [key, held] = entry;
    const bool kept =
        held.readers > 0 || prims_->materials.contains(pxr::SdfPath(key));
    if (kept) {
      return false;
    }
    textures_->Unread(held.textures);
    changes_->RemoveMaterial(held.id);
    ledger_->material_ids.Free(held.id);
    return true;
  });
}

std::optional<device::MaterialId> Materials::DisplayColorId() const {
  const auto held = ledger_->materials.find(std::string(kDisplayColorMaterial));
  if (held == ledger_->materials.end()) {
    return std::nullopt;
  }
  return held->second.id;
}

void Materials::Report(Warnings& warnings) const {
  MaterialTallies tallies;
  for (const auto& [path, material] : prims_->materials) {
    if (material->IsChanged() && material->translation().has_value()) {
      tallies.Add(path, *material->translation());
    }
  }
  tallies.Report(warnings);
}

// "1 mesh asks", "120 meshes ask".
std::string Meshes(std::size_t count, std::string_view singular_verb,
                   std::string_view plural_verb) {
  return count == 1 ? std::format("1 mesh {}", singular_verb)
                    : std::format("{} meshes {}", count, plural_verb);
}

// The meshes, as prototypes with their instances and their materials, and
// the tallies of those that are not, for their warnings.
class Geometry {
 public:
  Geometry(Ledger& ledger UMBRA_LIFETIMEBOUND,
           MotionTallies& motion UMBRA_LIFETIMEBOUND,
           Materials& materials UMBRA_LIFETIMEBOUND,
           ChangeSetBuilder& changes UMBRA_LIFETIMEBOUND)
      : ledger_(&ledger),
        motion_(&motion),
        materials_(&materials),
        changes_(&changes) {}

  // Adds, updates or removes the prototype of `mesh`.
  void Commit(const pxr::SdfPath& path, MeshPrim& mesh);
  void Add(const UnsupportedPrim& prim);
  // Removes the prototypes whose mesh is gone.
  void RemoveGone(const SyncedPrims& prims);

  // "warning: usd: 120 meshes ask for catmullClark subdivision, rendered
  // as their cage (first /World/Car/Body)", and so on.
  void Report(Warnings& warnings) const {
    ReportMeshes(warnings);
    ReportUnsupported(warnings);
  }

 private:
  // The keys of the materials of `mesh`: its own first, then those of its
  // GeomSubsets.
  [[nodiscard]] std::vector<std::string> MaterialKeys(
      const MeshPrim& mesh) const;
  // Gives the host copy of `mesh` its materials, and `held` their keys.
  void AssignMaterials(MeshPrim& mesh, HeldPrototype& held);
  // Removes the prototype `held`.
  void Remove(const HeldPrototype& held);
  // Updates the prototype `held` with what changed in `mesh`.
  void Update(HeldPrototype& held, MeshPrim& mesh);
  // Counts `mesh` in the tallies it belongs to.
  void Count(const pxr::SdfPath& path, const MeshPrim& mesh);
  // Whether `mesh` is a prototype: converted, with an instance or more.
  [[nodiscard]] static bool IsPlaced(const MeshPrim& mesh) {
    return mesh.state() == MeshState::kConverted && mesh.instance_count() > 0;
  }
  void ReportMeshes(Warnings& warnings) const;
  void ReportUnsupported(Warnings& warnings) const;

  Ledger* ledger_;
  MotionTallies* motion_;
  Materials* materials_;
  ChangeSetBuilder* changes_;
  std::map<std::string, Tally> subdivided_;
  Tally invalid_;
  std::map<pxr::TfToken, Tally> unsupported_;
  Tally mesh_lights_;
};

std::vector<std::string> Geometry::MaterialKeys(const MeshPrim& mesh) const {
  const DisplayColor display_color = mesh.display_color();
  std::vector<std::string> keys = {
      materials_->KeyOf(mesh.GetMaterialId(), display_color),
  };
  for (const MaterialSubset& subset : mesh.material_subsets()) {
    keys.push_back(materials_->KeyOf(subset.material, display_color));
  }
  return keys;
}

void Geometry::AssignMaterials(MeshPrim& mesh, HeldPrototype& held) {
  std::vector<std::string> keys = MaterialKeys(mesh);
  const std::vector<device::MaterialId> ids = materials_->Read(keys);
  materials_->Unread(std::exchange(held.materials, std::move(keys)));
  mesh.mesh().materials.assign(ids.begin(), ids.end());
}

void Geometry::Remove(const HeldPrototype& held) {
  materials_->Unread(held.materials);
  changes_->RemovePrototype(held.id);
  ledger_->prototype_ids.Free(held.id);
}

void Geometry::Count(const pxr::SdfPath& path, const MeshPrim& mesh) {
  if (mesh.state() == MeshState::kInvalid) {
    invalid_.Add(path, mesh.invalid_reason());
  }
  if (!IsPlaced(mesh)) {
    return;
  }
  if (!mesh.subdivision_scheme().empty()) {
    subdivided_[mesh.subdivision_scheme()].Add(path);
  }
  if (mesh.IsMeshLight()) {
    mesh_lights_.Add(path);
  }
  motion_->Add(path, mesh.motion_outcome());
}

void Geometry::Update(HeldPrototype& held, MeshPrim& mesh) {
  const device::ChangeFlags flags = mesh.changes();
  if (flags == device::ChangeFlags::kNone) {
    return;
  }
  if (device::HasAny(flags, device::ChangeFlags::kMaterials)) {
    AssignMaterials(mesh, held);
  }
  changes_->UpdatePrototype(held.id, mesh.mesh(), mesh.instances(), flags);
}

void Geometry::Commit(const pxr::SdfPath& path, MeshPrim& mesh) {
  Count(path, mesh);
  const auto held = ledger_->prototypes.find(path);
  if (!IsPlaced(mesh)) {
    if (held != ledger_->prototypes.end()) {
      Remove(held->second);
      ledger_->prototypes.erase(held);
    }
    return;
  }
  if (held != ledger_->prototypes.end()) {
    Update(held->second, mesh);
    return;
  }
  HeldPrototype& added = ledger_->prototypes[path];
  added.id = ledger_->prototype_ids.Take();
  AssignMaterials(mesh, added);
  changes_->AddPrototype(added.id, mesh.mesh(), mesh.instances());
}

void Geometry::RemoveGone(const SyncedPrims& prims) {
  std::erase_if(ledger_->prototypes, [&](const auto& entry) {
    if (prims.meshes.contains(entry.first)) {
      return false;
    }
    Remove(entry.second);
    return true;
  });
}

void Geometry::Add(const UnsupportedPrim& prim) {
  if (prim.IsSynced()) {
    unsupported_[prim.type()].Add(prim.GetId());
  }
}

void Geometry::ReportMeshes(Warnings& warnings) const {
  for (const auto& [scheme, tally] : subdivided_) {
    warnings.Warn(std::format("{} for {} subdivision, rendered as {} cage ({})",
                              Meshes(tally.count(), "asks", "ask"), scheme,
                              Plural(tally.count(), "its", "their"),
                              tally.First()));
  }
  if (invalid_.count() > 0) {
    warnings.Warn(std::format("{} invalid, dropped ({}: {})",
                              Meshes(invalid_.count(), "is", "are"),
                              invalid_.First(), invalid_.detail()));
  }
}

void Geometry::ReportUnsupported(Warnings& warnings) const {
  for (const auto& [type, tally] : unsupported_) {
    warnings.Warn(std::format("{} {} {} not supported yet; left out ({})",
                              tally.count(), type.GetString(),
                              Plural(tally.count(), "prim", "prims"),
                              tally.First()));
  }
  if (mesh_lights_.count() > 0) {
    warnings.Warn(
        std::format("{} not supported yet; {} rendered without emission ({})",
                    Plural(mesh_lights_.count(), "1 mesh light",
                           std::format("{} mesh lights", mesh_lights_.count())),
                    Plural(mesh_lights_.count(), "its mesh", "their meshes"),
                    mesh_lights_.First()));
  }
}

// The words of the warning about one LightIssue: "{count} {subject}
// {predicate} ({first}{: detail})", each for one light and for more.
struct IssueWords {
  LightIssue issue;
  std::array<std::string_view, 2> subject;
  std::array<std::string_view, 2> predicate;
};

constexpr std::array<IssueWords, 10> kIssueWords = {
    IssueWords{
        .issue = LightIssue::kFilters,
        .subject = {"light has", "lights have"},
        .predicate =
            {
                "light filters, which umbra does not take yet; ignored",
                "light filters, which umbra does not take yet; ignored",
            },
    },
    IssueWords{
        .issue = LightIssue::kIesProfileNotRead,
        .subject = {"light's IES profile is", "lights' IES profiles are"},
        .predicate =
            {
                "not read, rendered without it",
                "not read, rendered without them",
            },
    },
    IssueWords{
        .issue = LightIssue::kIesAngleScale,
        .subject = {"light rescales", "lights rescale"},
        .predicate =
            {
                "the angles of its IES profile (shaping:ies:angleScale), "
                "which umbra does not: lights are physical",
                "the angles of their IES profiles (shaping:ies:angleScale), "
                "which umbra does not: lights are physical",
            },
    },
    IssueWords{
        .issue = LightIssue::kNonPositiveTemperature,
        .subject = {"light enables", "lights enable"},
        .predicate =
            {
                "a color temperature not above 0 K; its color is used alone",
                "a color temperature not above 0 K; their color is used alone",
            },
    },
    IssueWords{
        .issue = LightIssue::kDiffuseSpecular,
        .subject = {"light scales", "lights scale"},
        .predicate =
            {
                "its diffuse or specular contribution, which umbra does not: "
                "lights are physical",
                "their diffuse or specular contribution, which umbra does not: "
                "lights are physical",
            },
    },
    IssueWords{
        .issue = LightIssue::kFocus,
        .subject = {"light focuses", "lights focus"},
        .predicate =
            {
                "its emission (shaping:focus), which umbra does not: lights "
                "are physical",
                "their emission (shaping:focus), which umbra does not: lights "
                "are physical",
            },
    },
    IssueWords{
        .issue = LightIssue::kConeSoftness,
        .subject = {"light softens", "lights soften"},
        .predicate =
            {
                "the edge of its cone, which umbra does not: lights are "
                "physical",
                "the edge of their cone, which umbra does not: lights are "
                "physical",
            },
    },
    IssueWords{
        .issue = LightIssue::kShadowControls,
        .subject = {"light authors", "lights author"},
        .predicate =
            {
                "shadow controls (ShadowAPI), which umbra does not take: "
                "lights are physical",
                "shadow controls (ShadowAPI), which umbra does not take: "
                "lights are physical",
            },
    },
    IssueWords{
        .issue = LightIssue::kColorNotConverted,
        .subject = {"light's color does", "lights' colors do"},
        .predicate =
            {
                "not convert into ACEScg, rendered white",
                "not convert into ACEScg, rendered white",
            },
    },
    IssueWords{
        .issue = LightIssue::kTextureNotRead,
        .subject = {"light texture is", "light textures are"},
        .predicate =
            {
                "not read, rendered with its light's color",
                "not read, rendered with their light's color",
            },
    },
};

// "warning: usd: 2 lights have light filters, which umbra does not take
// yet; ignored (first /World/key)", and so on.
void ReportLightIssue(LightIssue issue, const Tally& tally,
                      Warnings& warnings) {
  const auto* words = std::ranges::find(kIssueWords, issue, &IssueWords::issue);
  if (words == kIssueWords.end()) {
    return;
  }
  const std::size_t form = tally.count() == 1 ? 0 : 1;
  warnings.Warn(std::format("{} {} {} ({}{}{})", tally.count(),
                            words->subject.at(form), words->predicate.at(form),
                            tally.First(), tally.detail().empty() ? "" : ": ",
                            tally.detail()));
}

// The lights, those converted with an instance or more, updated whole when
// they sync, and the tallies of those that are not and of what they author
// that umbra does not take, for their warnings. A portal is committed only
// while a dome guides it, and a dome holds the ids of its portals.
class Lights {
 public:
  Lights(const SyncedPrims& prims UMBRA_LIFETIMEBOUND,
         Ledger& ledger UMBRA_LIFETIMEBOUND,
         MotionTallies& motion UMBRA_LIFETIMEBOUND,
         Textures& textures UMBRA_LIFETIMEBOUND,
         ChangeSetBuilder& changes UMBRA_LIFETIMEBOUND)
      : prims_(&prims),
        ledger_(&ledger),
        motion_(&motion),
        textures_(&textures),
        changes_(&changes) {}

  // Adds, updates or removes every light: the domes with portals last, once
  // their portals have ids.
  void CommitAll();
  // Removes the lights whose prim is gone.
  void RemoveGone(const SyncedPrims& prims);

  // "warning: usd: 2 lights have light filters, which umbra does not take
  // yet; ignored (first /World/key)", and so on.
  void Report(Warnings& warnings) const;

  // The error that stops the render: the first of the lights committed.
  [[nodiscard]] const std::optional<Error>& fatal() const UMBRA_LIFETIMEBOUND {
    return fatal_;
  }

 private:
  // Adds, updates or removes `light`.
  void Commit(const pxr::SdfPath& path, LightPrim& light);
  // Removes the light `held`.
  void Remove(const HeldLight& held);
  // Counts `light` in the tallies it belongs to, and its fatal error.
  void Count(const pxr::SdfPath& path, const LightPrim& light);
  // Gives the dome `light` the ids of its portals that are committed, and
  // tallies it when it targets a prim that is not a PortalLight.
  void AssignPortals(const pxr::SdfPath& path, LightPrim& light);
  // Whether `light` is committed: converted, with an instance or more,
  // and guided by a dome if it is a portal.
  [[nodiscard]] bool IsCommitted(const pxr::SdfPath& path,
                                 const LightPrim& light) const {
    return light.IsPlaced() &&
           (!light.IsPortal() || prims_->guided_portals.contains(path));
  }

  const SyncedPrims* prims_;
  Ledger* ledger_;
  MotionTallies* motion_;
  Textures* textures_;
  ChangeSetBuilder* changes_;
  std::map<LightIssue, Tally> issues_;
  std::map<std::string, Tally> unsupported_;
  // The portals no dome guides, and the domes that target a prim that is
  // not a PortalLight as a portal.
  Tally unguided_;
  Tally not_portals_;
  std::optional<Error> fatal_;
};

void Lights::Remove(const HeldLight& held) {
  textures_->Unread(held.textures);
  changes_->RemoveLight(held.id);
  ledger_->light_ids.Free(held.id);
}

void Lights::Count(const pxr::SdfPath& path, const LightPrim& light) {
  if (light.state() == LightState::kUnsupported) {
    unsupported_[light.unsupported_type()].Add(path);
  }
  if (!IsCommitted(path, light)) {
    if (light.IsPortal() && light.IsPlaced()) {
      unguided_.Add(path);
    }
    return;
  }
  motion_->Add(path, light.motion_outcome());
  if (!light.IsChanged()) {
    return;
  }
  if (light.fatal().has_value() && !fatal_.has_value()) {
    fatal_ = light.fatal();
  }
  for (const LightNote& note : light.notes()) {
    issues_[note.issue].Add(path, note.detail);
  }
}

void Lights::AssignPortals(const pxr::SdfPath& path, LightPrim& light) {
  std::vector<device::LightId> ids;
  std::optional<pxr::SdfPath> ignored;
  for (const pxr::SdfPath& target : light.portals()) {
    const auto portal = prims_->lights.find(target);
    if (portal == prims_->lights.end() || !portal->second->IsPortal()) {
      ignored = ignored.value_or(target);
      continue;
    }
    const auto held = ledger_->lights.find(target);
    if (prims_->guided_portals.contains(target) &&
        held != ledger_->lights.end()) {
      ids.push_back(held->second.id);
    }
  }
  if (ignored.has_value()) {
    not_portals_.Add(path, ignored->GetString());
  }
  light.AssignPortals(ids);
}

void Lights::CommitAll() {
  for (const auto& [path, light] : prims_->lights) {
    if (light->portals().empty()) {
      Commit(path, *light);
    }
  }
  for (const auto& [path, light] : prims_->lights) {
    if (!light->portals().empty()) {
      Commit(path, *light);
    }
  }
}

void Lights::Commit(const pxr::SdfPath& path, LightPrim& light) {
  Count(path, light);
  const auto held = ledger_->lights.find(path);
  if (!IsCommitted(path, light)) {
    if (held != ledger_->lights.end()) {
      Remove(held->second);
      ledger_->lights.erase(held);
    }
    return;
  }
  if (!light.IsChanged()) {
    return;
  }
  AssignPortals(path, light);
  std::vector<host::Texture> read = TexturesOf(light.light());
  textures_->Read(read);
  if (held == ledger_->lights.end()) {
    HeldLight& added = ledger_->lights[path];
    added.id = ledger_->light_ids.Take();
    added.textures = std::move(read);
    changes_->AddLight(added.id, light.light());
    return;
  }
  changes_->UpdateLight(held->second.id, light.light());
  textures_->Unread(std::exchange(held->second.textures, std::move(read)));
}

void Lights::RemoveGone(const SyncedPrims& prims) {
  std::erase_if(ledger_->lights, [&](const auto& entry) {
    if (prims.lights.contains(entry.first)) {
      return false;
    }
    Remove(entry.second);
    return true;
  });
}

void Lights::Report(Warnings& warnings) const {
  for (const auto& [type, tally] : unsupported_) {
    warnings.Warn(std::format(
        "{} {} {} not supported yet; left out ({})", tally.count(), type,
        Plural(tally.count(), "prim", "prims"), tally.First()));
  }
  for (const auto& [issue, tally] : issues_) {
    ReportLightIssue(issue, tally, warnings);
  }
  if (unguided_.count() > 0) {
    warnings.Warn(std::format(
        "{} {} in no rendered dome light's portals; left out ({})",
        unguided_.count(),
        Plural(unguided_.count(), "PortalLight is", "PortalLights are"),
        unguided_.First()));
  }
  if (not_portals_.count() > 0) {
    warnings.Warn(std::format(
        "{} {} a prim that is not a PortalLight as a portal; ignored ({}: {})",
        not_portals_.count(),
        Plural(not_portals_.count(), "dome light targets",
               "dome lights target"),
        not_portals_.First(), not_portals_.detail()));
  }
}

// Whether the light `prim` links geometry: whether hdsi gave its lightLink
// or its shadowLink a category, as it does to any collection but the
// default one of every prim.
bool IsLinked(const pxr::HdSceneIndexPrim& prim) {
  const pxr::HdContainerDataSourceHandle light =
      pxr::HdLightSchema::GetFromParent(prim.dataSource).GetContainer();
  if (light == nullptr) {
    return false;
  }
  const std::array<pxr::TfToken, 2> links = {
      pxr::HdTokens->lightLink,
      pxr::HdTokens->shadowLink,
  };
  return std::ranges::any_of(links, [&](const pxr::TfToken& link) {
    const auto category =
        pxr::HdTypedSampledDataSource<pxr::TfToken>::Cast(light->Get(link));
    return category != nullptr && !category->GetTypedValue(0.0F).IsEmpty();
  });
}

// "warning: usd: 3 lights link geometry, which umbra does not: every light
// lights everything (first /World/rim)"
// (docs/adr/0009-lights-light-everything.md): of the lights that synced
// since the last commit.
void ReportLightLinking(const pxr::HdSceneIndexBaseRefPtr& scene_index,
                        const SyncedPrims& prims, Warnings& warnings) {
  Tally linked;
  for (const auto& [path, light] : prims.lights) {
    if (light->IsChanged() && IsLinked(scene_index->GetPrim(path))) {
      linked.Add(path);
    }
  }
  if (linked.count() > 0) {
    warnings.Warn(std::format(
        "{} {} geometry, which umbra does not: every light lights everything "
        "({})",
        linked.count(), Plural(linked.count(), "light links", "lights link"),
        linked.First()));
  }
}

// The quantity of a raw var named `name`, or none when umbra has no such
// var.
std::optional<RenderVarSource> RawSource(std::string_view name) {
  static constexpr std::array<std::pair<std::string_view, RenderVarSource>, 4>
      kSources = {
          {
              {"color", RenderVarSource::kColor},
              {"depth", RenderVarSource::kDepth},
              {"normal", RenderVarSource::kNormal},
              {"albedo", RenderVarSource::kAlbedo},
          },
  };
  for (const auto& [known, source] : kSources) {
    if (known == name) {
      return source;
    }
  }
  return std::nullopt;
}

// The quantity of a var of `source_type` whose source is `source_name`, or
// none when umbra has no such var: an lpe or primvar var, unchecked, or a
// raw var umbra knows. An intrinsic var, which UsdRender leaves
// unimplemented, has none.
std::optional<RenderVarSource> SourceOf(const pxr::TfToken& source_type,
                                        std::string_view source_name) {
  if (source_type == pxr::UsdRenderTokens->lpe) {
    return RenderVarSource::kLpe;
  }
  if (source_type == pxr::UsdRenderTokens->primvar) {
    return RenderVarSource::kPrimvar;
  }
  if (source_type == pxr::UsdRenderTokens->raw) {
    return RawSource(source_name);
  }
  return std::nullopt;
}

// Whether `data_type` has half components: half, half3, color3h, ...
bool IsHalfType(std::string_view data_type) {
  return data_type.starts_with("half") || data_type.ends_with('h');
}

// The channel type of a var: half, unless its prim authors a dataType of
// more precision (float3, color3f, double), which gets float. The schema's
// fallback, color3f, is not authored: it gives half.
ChannelType ChannelTypeOf(const Var& var, const pxr::UsdStage& stage) {
  const pxr::UsdRenderVar prim(stage.GetPrimAtPath(var.varPath));
  const bool authored = prim.GetDataTypeAttr().HasAuthoredValue();
  return authored && !IsHalfType(var.dataType.GetString()) ? ChannelType::kFloat
                                                           : ChannelType::kHalf;
}

// The vars of `product` umbra renders, the others added to `unsupported`.
std::vector<RenderVar> ProductVars(const Product& product,
                                   const pxr::UsdStage& stage,
                                   UnsupportedVars& unsupported) {
  std::vector<RenderVar> vars;
  for (const Var& var : product.renderVars) {
    // A var without a sourceName is the quantity it is named after.
    const std::string name = var.varPath.GetName();
    const std::string source_name =
        var.sourceName.empty() ? name : var.sourceName;
    const auto source = SourceOf(var.sourceType, source_name);
    if (!source.has_value()) {
      unsupported.Add(source_name, product.productPath);
      continue;
    }
    const bool named = *source == RenderVarSource::kLpe ||
                       *source == RenderVarSource::kPrimvar;
    vars.push_back({
        .name = name,
        .source = *source,
        .source_name = named ? source_name : std::string(),
        .type = ChannelTypeOf(var, stage),
    });
  }
  return vars;
}

// The policy of a conformPolicy token: expandAperture, the fallback, for an
// unknown one.
AspectRatioConformPolicy ConformPolicy(const pxr::TfToken& token) {
  using enum AspectRatioConformPolicy;
  if (token == pxr::UsdRenderTokens->cropAperture) {
    return kCropAperture;
  }
  if (token == pxr::UsdRenderTokens->adjustApertureWidth) {
    return kAdjustApertureWidth;
  }
  if (token == pxr::UsdRenderTokens->adjustApertureHeight) {
    return kAdjustApertureHeight;
  }
  if (token == pxr::UsdRenderTokens->adjustPixelAspectRatio) {
    return kAdjustPixelAspectRatio;
  }
  return kExpandAperture;
}

// A product with the framing of `base` and no vars, for `file`.
RenderProduct Framed(std::string file, device::CameraId camera,
                     const SettingsBase& base) {
  return {
      .file = std::move(file),
      .camera = camera,
      .resolution = base.resolution,
      .pixel_aspect_ratio = base.pixel_aspect_ratio,
      .aspect_ratio_conform_policy =
          ConformPolicy(base.aspect_ratio_conform_policy),
      .data_window_ndc = base.data_window_ndc,
      .motion_blur = base.motion_blur,
      .depth_of_field = base.depth_of_field,
      .vars = {},
  };
}

// A data window as xmin, ymin, xmax, ymax.
std::array<float, 4> DataWindow(const pxr::GfRange2f& window) {
  const std::array<float, 2> min = Components<float, 2>(window.GetMin());
  const std::array<float, 2> max = Components<float, 2>(window.GetMax());
  return {min.at(0), min.at(1), max.at(0), max.at(1)};
}

// The framing of a flattened product, which inherits the settings'.
SettingsBase ProductBase(const Product& product) {
  return {
      .resolution = Components<std::int32_t, 2>(product.resolution),
      .pixel_aspect_ratio = product.pixelAspectRatio,
      .aspect_ratio_conform_policy = product.aspectRatioConformPolicy,
      .data_window_ndc = DataWindow(product.dataWindowNDC),
      .motion_blur = product.disableMotionBlur ? MotionBlur::kDisabled
                                               : MotionBlur::kEnabled,
      .depth_of_field = product.disableDepthOfField ? DepthOfField::kDisabled
                                                    : DepthOfField::kEnabled,
  };
}

// Converts the products of the settings, as the render writes them.
class ProductConverter {
 public:
  ProductConverter(const CommitInputs& inputs UMBRA_LIFETIMEBOUND,
                   const pxr::UsdStage& stage UMBRA_LIFETIMEBOUND,
                   Cameras& cameras UMBRA_LIFETIMEBOUND)
      : inputs_(&inputs), stage_(&stage), cameras_(&cameras) {}

  std::expected<RenderProduct, Error> Convert(const Product& product);

  // The product of settings without products: one color var.
  std::expected<RenderProduct, Error> Implicit(device::CameraId camera,
                                               const SettingsBase& base);

  void Report(Warnings& warnings) const { unsupported_.Report(warnings); }

 private:
  const CommitInputs* inputs_;
  const pxr::UsdStage* stage_;
  Cameras* cameras_;
  UnsupportedVars unsupported_;
};

std::expected<RenderProduct, Error> ProductConverter::Convert(
    const Product& product) {
  const std::string owner =
      std::format("render product {}", product.productPath.GetString());
  if (product.renderVars.empty()) {
    return std::unexpected(Error(ErrorCode::kInvalidArgument,
                                 std::format("{} has no orderedVars", owner)));
  }
  const auto camera = cameras_->Index(product.cameraPath, owner);
  if (!camera.has_value()) {
    return std::unexpected(camera.error());
  }
  std::string file = ExpandFrame(product.name.GetString(), inputs_->frame);
  if (auto prepared = PrepareOutput(file); !prepared.has_value()) {
    return std::unexpected(prepared.error());
  }
  RenderProduct converted =
      Framed(std::move(file), *camera, ProductBase(product));
  converted.vars = ProductVars(product, *stage_, unsupported_);
  if (converted.vars.empty()) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("{} has no var umbra renders", owner)));
  }
  return converted;
}

std::expected<RenderProduct, Error> ProductConverter::Implicit(
    device::CameraId camera, const SettingsBase& base) {
  std::string file = ImplicitProductFile(inputs_->scene_path, inputs_->frame);
  if (auto prepared = PrepareOutput(file); !prepared.has_value()) {
    return std::unexpected(prepared.error());
  }
  RenderProduct product = Framed(std::move(file), camera, base);
  product.vars.push_back({
      .name = "color",
      .source = RenderVarSource::kColor,
      .source_name = {},
      .type = ChannelType::kHalf,
  });
  return product;
}

// The products of the settings, or their implicit one.
std::expected<std::vector<RenderProduct>, Error> Products(
    const RenderSettingsPrim& settings, device::CameraId camera,
    ProductConverter& converter) {
  std::vector<RenderProduct> products;
  if (settings.GetRenderProducts().empty()) {
    auto implicit = converter.Implicit(camera, settings.base());
    if (!implicit.has_value()) {
      return std::unexpected(implicit.error());
    }
    products.push_back(std::move(*implicit));
    return products;
  }
  for (const Product& product : settings.GetRenderProducts()) {
    auto converted = converter.Convert(product);
    if (!converted.has_value()) {
      return std::unexpected(converted.error());
    }
    products.push_back(std::move(*converted));
  }
  return products;
}

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
      "{} triangle{} in {} mesh{}, {} primvar{}, {} instance{} of {} "
      "prototype{}",
      core::FormatCount(triangles), triangles == 1 ? "" : "s",
      core::FormatCount(meshes), meshes == 1 ? "" : "es",
      core::FormatCount(primvars.size()), primvars.size() == 1 ? "" : "s",
      core::FormatCount(instances), instances == 1 ? "" : "s",
      core::FormatCount(meshes), meshes == 1 ? "" : "s");
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
                                            const TextureIds& textures) {
  std::set<device::TextureId> read;
  for (const device::MaterialNetwork& material : changes.materials.added) {
    for (const device::MaterialNode& node : material.nodes) {
      InsertTextureIds(node, read);
    }
  }
  std::set<std::string_view> files;
  for (const auto& [texture, texture_id] : textures) {
    if (read.contains(texture_id)) {
      files.insert(texture.file);
    }
  }
  return files;
}

// "57 materials (MaterialX 40, UsdPreviewSurface 15, OSL 2), 210 textures,
// 300 meshes on displayColor": those added, the textures counted by file.
std::string DescribeMaterials(const device::ChangeSet& changes,
                              const TextureIds& textures,
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
  return std::format(
      "{} material{}{}, {} texture{}, {} mesh{} on displayColor",
      core::FormatCount(materials), materials == 1 ? "" : "s",
      counts.empty() ? "" : std::format(" ({})", counts),
      core::FormatCount(texture_count), texture_count == 1 ? "" : "s",
      core::FormatCount(on_display_color), on_display_color == 1 ? "" : "es");
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
  return std::format("{} light{}{}", core::FormatCount(lights),
                     lights == 1 ? "" : "s",
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
  return std::format("shutter {}..{}, {} moving (max {} sample{})",
                     shutter.open, shutter.close,
                     core::FormatCount(motion.moving), motion.samples,
                     motion.samples == 1 ? "" : "s");
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

// What a commit added, which the debug line counts, besides the change set.
struct Description {
  // The path of the camera the settings see through.
  std::string_view camera;
  const TextureIds* textures;
  std::optional<device::MaterialId> display_color;
};

// "settings /Render/main, camera /World/cam, 1 product: render/beauty.0012.exr
// [color, depth], 1.24M triangles in 3.4k meshes, 18 primvars, 2.1M
// instances of 3.4k prototypes, 57 materials (MaterialX 40,
// UsdPreviewSurface 15, OSL 2), 210 textures, 300 meshes on displayColor,
// 14 lights (rect 8, sphere 4, distant 1, dome 1), shutter -0.25..0.25, 1.2k
// moving (max 4 samples), 2 prims updated, 0 removed": what it added, then
// how many prims it updated and removed.
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
      "settings {}, camera {}, {} product{}: {}, {}, {}, {}, {}, {} prim{} "
      "updated, {} removed",
      settings.path, description.camera, count, count == 1 ? "" : "s", products,
      DescribeGeometry(changes),
      DescribeMaterials(changes, *description.textures,
                        description.display_color),
      DescribeLights(changes), DescribeMotion(changes, shutter),
      core::FormatCount(updated), updated == 1 ? "" : "s",
      core::FormatCount(CountRemoved(changes)));
}

// Commits `changes` to every device of `devices` in parallel, each on a host
// thread of its own, and gives each one's result, in order, once all are
// done.
std::vector<std::expected<void, Error>> CommitInParallel(
    const device::ChangeSet& changes,
    std::span<device::Device* const> devices) {
  std::vector<std::expected<void, Error>> results(devices.size());
  std::vector<std::jthread> threads;
  threads.reserve(devices.size());
  for (const auto& [device, result] : std::views::zip(devices, results)) {
    threads.emplace_back([&changes, device, &result] {
      core::ConfigureThreadFloatingPoint();
      result = device->Commit(changes);
    });
  }
  // Joins every thread.
  threads.clear();
  return results;
}

// Commits `changes` to every device of `devices` in parallel: fails with the
// error of the first, in order, that fails, naming it.
std::expected<void, Error> CommitToDevices(
    const device::ChangeSet& changes,
    std::span<device::Device* const> devices) {
  const std::vector<std::expected<void, Error>> results =
      CommitInParallel(changes, devices);
  for (const auto& [device, committed] : std::views::zip(devices, results)) {
    if (!committed.has_value()) {
      return std::unexpected(Error(committed.error().code(),
                                   std::format("device {}: {}", device->name(),
                                               committed.error().message())));
    }
  }
  return {};
}

// One commit: the change set it builds from the synced prims and the
// ledger, and the warnings it reports.
class FrameCommit {
 public:
  FrameCommit(const CommitInputs& inputs UMBRA_LIFETIMEBOUND,
              const CommitScene& scene UMBRA_LIFETIMEBOUND,
              const SyncedPrims& prims UMBRA_LIFETIMEBOUND,
              Ledger& ledger UMBRA_LIFETIMEBOUND)
      : inputs_(&inputs),
        scene_(&scene),
        prims_(&prims),
        ledger_(&ledger),
        warnings_(ledger.warnings),
        changes_(ledger.textures),
        textures_(ledger, changes_),
        materials_(prims, ledger, changes_, textures_),
        cameras_(prims, ledger, motion_, changes_,
                 ledger.shutter == prims.shutter ? ShutterChange::kSame
                                                 : ShutterChange::kChanged) {}
  FrameCommit(const FrameCommit&) = delete;
  FrameCommit& operator=(const FrameCommit&) = delete;
  FrameCommit(FrameCommit&&) = delete;
  FrameCommit& operator=(FrameCommit&&) = delete;
  ~FrameCommit() = default;

  // The render settings of `settings` and the frame parameters, with the
  // cameras they see through; the description is left empty.
  std::expected<Committed, Error> CommitSettings(
      const RenderSettingsPrim& settings);

  // Adds to the change set what changed in what the frame renders, its
  // geometry with its materials and its lights, the warnings about them
  // reported; or gives the error that stops the render.
  std::expected<void, Error> CommitContent();

  // The change set, valid while the committer and the host copies live.
  [[nodiscard]] device::ChangeSet Build() const UMBRA_LIFETIMEBOUND {
    return changes_.Build(ledger_->id_counts());
  }

  // What `change_set` commits, with `settings`: see Describe.
  [[nodiscard]] std::string DescribeCommit(
      const RenderSettings& settings,
      const device::ChangeSet& change_set) const;

 private:
  // Counts the motion of the instancers that synced.
  void TallyInstancers();

  const CommitInputs* inputs_;
  const CommitScene* scene_;
  const SyncedPrims* prims_;
  Ledger* ledger_;
  Warnings warnings_;
  ChangeSetBuilder changes_;
  Textures textures_;
  Materials materials_;
  MotionTallies motion_;
  Cameras cameras_;
  // The path of the camera the settings see through.
  std::string camera_;
};

std::expected<Committed, Error> FrameCommit::CommitSettings(
    const RenderSettingsPrim& settings) {
  const pxr::SdfPath camera_path = settings.CameraPath();
  const auto camera = cameras_.Index(
      camera_path,
      std::format("render settings {}", inputs_->settings.GetString()));
  if (!camera.has_value()) {
    return std::unexpected(camera.error());
  }
  ProductConverter converter(*inputs_, *scene_->stage, cameras_);
  auto products = Products(settings, *camera, converter);
  converter.Report(warnings_);
  if (!products.has_value()) {
    return std::unexpected(products.error());
  }
  cameras_.RemoveUnseen();
  camera_ = camera_path.GetString();
  return Committed{
      .settings =
          {
              .path = inputs_->settings.GetString(),
              .camera = *camera,
              .products = std::move(*products),
          },
      .frame = {.shutter = prims_->shutter, .camera = *camera},
      .description = {},
  };
}

void FrameCommit::TallyInstancers() {
  for (const auto& [path, instancer] : prims_->instancers) {
    if (instancer->IsSynced()) {
      motion_.Add(path, instancer->motion_outcome());
    }
  }
}

std::expected<void, Error> FrameCommit::CommitContent() {
  materials_.CommitPrims();
  Lights lights(*prims_, *ledger_, motion_, textures_, changes_);
  lights.CommitAll();
  lights.RemoveGone(*prims_);
  Geometry geometry(*ledger_, motion_, materials_, changes_);
  for (const auto& [path, mesh] : prims_->meshes) {
    geometry.Commit(path, *mesh);
  }
  geometry.RemoveGone(*prims_);
  materials_.RemoveUnread();
  for (const auto& [path, prim] : prims_->unsupported) {
    geometry.Add(*prim);
  }
  TallyInstancers();
  lights.Report(warnings_);
  geometry.Report(warnings_);
  materials_.Report(warnings_);
  motion_.Report(warnings_);
  if (auto fatal = materials_.fatal().or_else([&] { return lights.fatal(); });
      fatal.has_value()) {
    return std::unexpected(std::move(*fatal));
  }
  ReportLightLinking(scene_->scene_index, *prims_, warnings_);
  return {};
}

std::string FrameCommit::DescribeCommit(
    const RenderSettings& settings, const device::ChangeSet& change_set) const {
  return Describe(settings, change_set, prims_->shutter,
                  {
                      .camera = camera_,
                      .textures = &ledger_->textures,
                      .display_color = materials_.DisplayColorId(),
                  });
}

}  // namespace

std::expected<Committed, Error> Commit(
    const CommitInputs& inputs, const CommitScene& scene, SyncedPrims& prims,
    Ledger& ledger, std::span<device::Device* const> devices) {
  const auto found = prims.settings.find(inputs.settings);
  if (found == prims.settings.end()) {
    return std::unexpected(
        Error(ErrorCode::kNotFound, std::format("no RenderSettings prim at {}",
                                                inputs.settings.GetString())));
  }
  FrameCommit commit(inputs, scene, prims, ledger);
  auto committed = commit.CommitSettings(*found->second);
  if (!committed.has_value()) {
    return committed;
  }
  if (auto content = commit.CommitContent(); !content.has_value()) {
    return std::unexpected(std::move(content.error()));
  }
  const device::ChangeSet change_set = commit.Build();
  committed->description =
      commit.DescribeCommit(committed->settings, change_set);
  if (auto done = CommitToDevices(change_set, devices); !done.has_value()) {
    return std::unexpected(std::move(done.error()));
  }
  ledger.Recycle();
  ledger.shutter = prims.shutter;
  return committed;
}

}  // namespace umbra::ext::usd::internal
