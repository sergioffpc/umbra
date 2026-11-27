// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The materials a commit gives the devices: every material prim, and
// umbra's two while a prototype reads them.

#ifndef UMBRA_DELEGATE_COMMIT_MATERIALS_H_
#define UMBRA_DELEGATE_COMMIT_MATERIALS_H_

#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "commit_textures.h"
#include "commit_warnings.h"
#include "host_copy.h"
#include "hydra.h"
#include "ledger.h"
#include "material.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/render_data.h"
#include "views.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

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
  [[nodiscard]] const std::optional<core::Error>& fatal() const
      UMBRA_LIFETIMEBOUND {
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
  // outlive the commit, and gives it.
  HeldMaterial& Commit(const std::string& key,
                       const host::MaterialNetwork& network);
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
  std::optional<core::Error> fatal_;
};

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_COMMIT_MATERIALS_H_
