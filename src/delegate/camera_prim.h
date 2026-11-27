// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// A Camera prim, as Hydra gives it, synced, then placed once the shutter
// the render uses is known: converted into its host copy, in meters.

#ifndef UMBRA_DELEGATE_CAMERA_PRIM_H_
#define UMBRA_DELEGATE_CAMERA_PRIM_H_

#include <pxr/base/gf/matrix4d.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <cstdint>
#include <span>

#include "host_copy.h"
#include "motion_samples.h"
#include "sync_counter.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

namespace pxr = PXR_NS;

// A Camera prim, synced, with the samples of its transform over its own
// shutter: the cameras sync before the shutter the render uses is known.
// Once it is, the commit places the camera: converts it into its host copy,
// in meters, its transform resampled over that shutter.
class CameraPrim final : public pxr::HdCamera {
 public:
  // Counts its sync into `counter`, unless it is a fallback prim; its lengths
  // are scaled by `meters_per_unit`.
  CameraPrim(const pxr::SdfPath& prim_id, SyncCounter* UMBRA_NULLABLE counter,
             double meters_per_unit);

  void Sync(pxr::HdSceneDelegate* scene_delegate,
            pxr::HdRenderParam* UMBRA_NULLABLE render_param,
            pxr::HdDirtyBits* dirty_bits) override;

  // Converts it into its host copy, its transform resampled over `shutter`.
  void Place(const host::Shutter& shutter);

  // It, in meters, its transform resampled over `shutter`.
  [[nodiscard]] host::Camera Sampled(const host::Shutter& shutter) const;

  // It at the frame, in meters, with the one transform it has at the frame.
  [[nodiscard]] host::Camera AtFrame() const;

  // Its host copy, once placed.
  [[nodiscard]] const host::Camera& camera() const UMBRA_LIFETIMEBOUND {
    return camera_;
  }
  // What resampling did with its motion, once placed.
  [[nodiscard]] MotionOutcome motion_outcome() const { return motion_outcome_; }
  // Whether it synced since its host copy was last committed.
  [[nodiscard]] bool IsChanged() const { return changed_ == Changed::kYes; }
  // Frees its host copy, once committed.
  void FreeHostCopy();

 private:
  enum class Changed : std::uint8_t { kNo, kYes };

  // It, in meters, with the motion samples of its transform
  // `camera_to_world`, in scene units.
  [[nodiscard]] host::Camera Converted(
      std::span<const pxr::GfMatrix4d> camera_to_world) const;

  SyncCounter* UMBRA_NULLABLE counter_;
  double meters_per_unit_;
  Changed changed_ = Changed::kNo;
  // The samples of its transform over its own shutter, in scene units: its
  // transform at the frame alone when that shutter spans no interval.
  TimedSamples<pxr::GfMatrix4d> transform_samples_;
  MotionOutcome motion_outcome_ = MotionOutcome::kSampled;
  host::Camera camera_;
};

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_CAMERA_PRIM_H_
