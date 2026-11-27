// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The cameras a commit gives the devices: those the render settings and
// their products see through.

#ifndef UMBRA_DELEGATE_COMMIT_CAMERAS_H_
#define UMBRA_DELEGATE_COMMIT_CAMERAS_H_

#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <cstdint>
#include <expected>
#include <map>
#include <string_view>

#include "commit_warnings.h"
#include "hydra.h"
#include "ledger.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/render_data.h"
#include "views.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

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
  std::expected<device::CameraId, core::Error> Index(const pxr::SdfPath& path,
                                                     std::string_view owner);

  // Removes the cameras held that no longer are seen through.
  void RemoveUnseen();

  // The camera at `path`, which Index gave an id.
  [[nodiscard]] const CameraPrim& Prim(const pxr::SdfPath& path) const
      UMBRA_LIFETIMEBOUND {
    return *prims_->cameras.at(path);
  }

 private:
  const SyncedPrims* prims_;
  Ledger* ledger_;
  MotionTallies* motion_;
  ChangeSetBuilder* changes_;
  ShutterChange shutter_;
  // The cameras seen through by this commit.
  std::map<pxr::SdfPath, device::CameraId> seen_;
};

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_COMMIT_CAMERAS_H_
