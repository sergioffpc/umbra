// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The lights a commit gives the devices: those placed, and the PortalLights
// a dome light committed targets.

#ifndef UMBRA_DELEGATE_COMMIT_LIGHTS_H_
#define UMBRA_DELEGATE_COMMIT_LIGHTS_H_

#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <map>
#include <optional>
#include <string>

#include "commit_textures.h"
#include "commit_warnings.h"
#include "hydra.h"
#include "ledger.h"
#include "light.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "views.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

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
  [[nodiscard]] const std::optional<core::Error>& fatal() const
      UMBRA_LIFETIMEBOUND {
    return fatal_;
  }

 private:
  // Adds, updates or removes `light`.
  void Commit(const pxr::SdfPath& path, LightPrim& light);
  // Gives the devices no more the light `held`.
  void Release(const HeldLight& held);
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
  std::optional<core::Error> fatal_;
};

// "warning: usd: 3 lights link geometry, which umbra does not: every light
// lights everything (first /World/rim)"
// (docs/adr/0009-lights-light-everything.md): of the lights that synced
// since the last commit, as `scene_index` gives them.
void ReportLightLinking(const pxr::HdSceneIndexBaseRefPtr& scene_index,
                        const SyncedPrims& prims, Warnings& warnings);

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_COMMIT_LIGHTS_H_
