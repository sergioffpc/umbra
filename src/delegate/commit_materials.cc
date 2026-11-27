// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "commit_materials.h"

#include <pxr/usd/sdf/path.h>

#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <variant>
#include <vector>

#include "commit_warnings.h"
#include "host_copy.h"
#include "hydra.h"
#include "ledger.h"
#include "material.h"
#include "plural.h"
#include "umbra/device/render_data.h"

namespace umbra::delegate::internal {
namespace {

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

}  // namespace

HeldMaterial& Materials::Commit(const std::string& key,
                                const host::MaterialNetwork& network) {
  const auto [entry, added] = ledger_->materials.Hold(key);
  HeldMaterial& held = entry.second;
  textures_->Swap(held.textures, TexturesOf(network));
  if (added) {
    changes_->AddMaterial(held.id, network);
  } else {
    changes_->UpdateMaterial(held.id, network);
  }
  return held;
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
    HeldMaterial* held = ledger_->materials.Find(key);
    if (held == nullptr) {
      held = &Commit(key, made_.emplace_back(key == kDisplayColorMaterial
                                                 ? DisplayColorMaterial()
                                                 : FallbackMaterial()));
    }
    ++held->readers;
    ids.push_back(held->id);
  }
  return ids;
}

void Materials::Unread(const std::vector<std::string>& keys) {
  for (const std::string& key : keys) {
    --ledger_->materials.at(key).readers;
  }
}

void Materials::RemoveUnread() {
  ledger_->materials.RemoveIf(
      [this](const std::string& key, const HeldMaterial& held) {
        return held.readers == 0 &&
               !prims_->materials.contains(pxr::SdfPath(key));
      },
      [this](const HeldMaterial& held) {
        textures_->Unread(held.textures);
        changes_->RemoveMaterial(held.id);
      });
}

std::optional<device::MaterialId> Materials::DisplayColorId() const {
  const HeldMaterial* held =
      ledger_->materials.Find(std::string(kDisplayColorMaterial));
  if (held == nullptr) {
    return std::nullopt;
  }
  return held->id;
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

}  // namespace umbra::delegate::internal
