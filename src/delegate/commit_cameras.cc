// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "commit_cameras.h"

#include <pxr/usd/sdf/path.h>

#include <expected>
#include <format>
#include <string_view>

#include "hydra.h"
#include "ledger.h"
#include "motion_samples.h"
#include "umbra/core/error.h"
#include "umbra/device/camera_rays.h"
#include "umbra/device/render_data.h"

namespace umbra::delegate::internal {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

// A camera model holds every motion sample of a camera's transform.
static_assert(device::kMaxCameraSamples >= kMaxMotionSamples);

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
  const auto [held, added] = ledger_->cameras.Hold(path);
  const device::CameraId camera_id = held.second.id;
  if (added) {
    camera.Place(prims_->shutter);
    changes_->AddCamera(camera_id, camera.camera());
  } else if (camera.IsChanged() || shutter_ == ShutterChange::kChanged) {
    camera.Place(prims_->shutter);
    changes_->UpdateCamera(camera_id, camera.camera());
  }
  motion_->Add(path, camera.motion_outcome());
  seen_.emplace(path, camera_id);
  return camera_id;
}

void Cameras::RemoveUnseen() {
  ledger_->cameras.RemoveMissing(seen_, [this](const HeldCamera& held) {
    changes_->RemoveCamera(held.id);
  });
}

}  // namespace umbra::delegate::internal
