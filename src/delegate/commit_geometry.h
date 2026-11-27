// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The geometry a commit gives the devices: each mesh placed, as a prototype
// with its instances and its materials.

#ifndef UMBRA_DELEGATE_COMMIT_GEOMETRY_H_
#define UMBRA_DELEGATE_COMMIT_GEOMETRY_H_

#include <pxr/base/tf/token.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <map>
#include <string>
#include <vector>

#include "commit_materials.h"
#include "commit_warnings.h"
#include "hydra.h"
#include "ledger.h"
#include "light.h"
#include "umbra/core/safety.h"
#include "views.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

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
  // Counts `prim`, of a type umbra does not render, when it synced.
  void Add(const UnsupportedPrim& prim);
  // Removes the prototypes whose mesh is gone.
  void RemoveGone(const SyncedPrims& prims);

  // "warning: usd: 3 meshes are invalid, dropped (first /World/Car/Body:
  // ...)", and so on.
  void Report(Warnings& warnings) const;

 private:
  // The keys of the materials of `mesh`: its own first, then those of its
  // GeomSubsets.
  [[nodiscard]] std::vector<std::string> MaterialKeys(
      const MeshPrim& mesh) const;
  // Gives the host copy of `mesh` its materials, and `held` their keys.
  void AssignMaterials(MeshPrim& mesh, HeldPrototype& held);
  // Gives the devices no more the prototype `held`.
  void Release(const HeldPrototype& held);
  // Updates the prototype `held` with what changed in `mesh`.
  void Update(HeldPrototype& held, MeshPrim& mesh);
  // Counts `mesh` in the tallies it belongs to.
  void Count(const pxr::SdfPath& path, const MeshPrim& mesh);
  // Whether `mesh` is a prototype: converted, with an instance or more.
  [[nodiscard]] static bool IsPlaced(const MeshPrim& mesh) {
    return mesh.state() == MeshState::kConverted && mesh.instance_count() > 0;
  }

  Ledger* ledger_;
  MotionTallies* motion_;
  Materials* materials_;
  ChangeSetBuilder* changes_;
  Tally invalid_;
  std::map<pxr::TfToken, Tally> unsupported_;
  // The issues of the mesh lights whose emission changed.
  std::map<LightIssue, Tally> emission_issues_;
};

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_COMMIT_GEOMETRY_H_
