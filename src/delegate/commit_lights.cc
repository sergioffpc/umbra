// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "commit_lights.h"

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/path.h>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "commit_warnings.h"
#include "host_copy.h"
#include "hydra.h"
#include "ledger.h"
#include "light.h"
#include "plural.h"
#include "umbra/device/render_data.h"

namespace umbra::delegate::internal {
namespace {

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

}  // namespace

void Lights::Release(const HeldLight& held) {
  textures_->Unread(held.textures);
  changes_->RemoveLight(held.id);
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
    const HeldLight* held = ledger_->lights.Find(target);
    if (prims_->guided_portals.contains(target) && held != nullptr) {
      ids.push_back(held->id);
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
  if (!IsCommitted(path, light)) {
    ledger_->lights.Remove(path,
                           [this](const HeldLight& held) { Release(held); });
    return;
  }
  if (!light.IsChanged()) {
    return;
  }
  AssignPortals(path, light);
  const auto [entry, added] = ledger_->lights.Hold(path);
  HeldLight& held = entry.second;
  textures_->Swap(held.textures, TexturesOf(light.light()));
  if (added) {
    changes_->AddLight(held.id, light.light());
  } else {
    changes_->UpdateLight(held.id, light.light());
  }
}

void Lights::RemoveGone(const SyncedPrims& prims) {
  ledger_->lights.RemoveMissing(
      prims.lights, [this](const HeldLight& held) { Release(held); });
}

void Lights::Report(Warnings& warnings) const {
  for (const auto& [type, tally] : unsupported_) {
    ReportUnsupported(type, tally, warnings);
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

}  // namespace umbra::delegate::internal
