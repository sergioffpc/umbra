// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "commit_geometry.h"

#include <pxr/usd/sdf/path.h>

#include <format>
#include <string>
#include <utility>
#include <vector>

#include "commit_warnings.h"
#include "hydra.h"
#include "ledger.h"
#include "light.h"
#include "plural.h"
#include "umbra/device/change_set.h"
#include "umbra/device/render_data.h"

namespace umbra::delegate::internal {

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

void Geometry::Release(const HeldPrototype& held) {
  materials_->Unread(held.materials);
  changes_->RemovePrototype(held.id);
}

void Geometry::Count(const pxr::SdfPath& path, const MeshPrim& mesh) {
  if (mesh.state() == MeshState::kInvalid) {
    invalid_.Add(path, mesh.invalid_reason());
  }
  if (!IsPlaced(mesh)) {
    return;
  }
  if (device::HasAny(mesh.changes(), device::ChangeFlags::kEmission)) {
    for (const LightNote& note : mesh.emission_notes()) {
      emission_issues_[note.issue].Add(path, note.detail);
    }
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
  if (!IsPlaced(mesh)) {
    ledger_->prototypes.Remove(
        path, [this](const HeldPrototype& held) { Release(held); });
    return;
  }
  const auto [entry, added] = ledger_->prototypes.Hold(path);
  HeldPrototype& held = entry.second;
  if (!added) {
    Update(held, mesh);
    return;
  }
  AssignMaterials(mesh, held);
  changes_->AddPrototype(held.id, mesh.mesh(), mesh.instances());
}

void Geometry::RemoveGone(const SyncedPrims& prims) {
  ledger_->prototypes.RemoveMissing(
      prims.meshes, [this](const HeldPrototype& held) { Release(held); });
}

void Geometry::Add(const UnsupportedPrim& prim) {
  if (prim.IsSynced()) {
    unsupported_[prim.type()].Add(prim.GetId());
  }
}

void Geometry::Report(Warnings& warnings) const {
  if (invalid_.count() > 0) {
    warnings.Warn(std::format("{} {} invalid, dropped ({}: {})",
                              invalid_.count(),
                              Plural(invalid_.count(), "mesh is", "meshes are"),
                              invalid_.First(), invalid_.detail()));
  }
  for (const auto& [type, tally] : unsupported_) {
    ReportUnsupported(type.GetString(), tally, warnings);
  }
  for (const auto& [issue, tally] : emission_issues_) {
    ReportLightIssue(issue, tally, warnings, "mesh ");
  }
}

}  // namespace umbra::delegate::internal
