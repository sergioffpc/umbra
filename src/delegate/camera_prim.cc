// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "camera_prim.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range1f.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/usd/sdf/path.h>

#include <array>
#include <span>
#include <utility>

#include "host_copy.h"
#include "mesh.h"
#include "motion.h"
#include "motion_samples.h"
#include "sync_counter.h"
#include "umbra/core/safety.h"

namespace umbra::delegate::internal {

CameraPrim::CameraPrim(const pxr::SdfPath& prim_id,
                       SyncCounter* UMBRA_NULLABLE counter,
                       double meters_per_unit)
    : pxr::HdCamera(prim_id),
      counter_(counter),
      meters_per_unit_(meters_per_unit) {}

void CameraPrim::Sync(pxr::HdSceneDelegate* scene_delegate,
                      pxr::HdRenderParam* UMBRA_NULLABLE render_param,
                      pxr::HdDirtyBits* dirty_bits) {
  pxr::HdCamera::Sync(scene_delegate, render_param, dirty_bits);
  const host::Shutter shutter{
      .open = GetShutterOpen(),
      .close = GetShutterClose(),
  };
  transform_samples_ = {.times = {0.0F}, .values = {GetTransform()}};
  if (IsMotionBlurred(shutter) && !GetId().IsEmpty()) {
    TimedSamples<pxr::GfMatrix4d> samples =
        SampleTransform(*scene_delegate, GetId(), shutter);
    if (!samples.values.empty()) {
      transform_samples_ = std::move(samples);
    }
  }
  changed_ = Changed::kYes;
  CountSync(counter_, GetId());
}

host::Camera CameraPrim::Converted(
    std::span<const pxr::GfMatrix4d> camera_to_world) const {
  // Lengths are in scene units, the apertures and focal length too once
  // Hydra has read them.
  const auto meters = [this](float length) {
    return static_cast<double>(length) * meters_per_unit_;
  };
  const pxr::GfRange1f& clipping = GetClippingRange();
  return {
      .path = GetId().GetString(),
      .projection = GetProjection() == pxr::HdCamera::Orthographic
                        ? host::Projection::kOrthographic
                        : host::Projection::kPerspective,
      .camera_to_world = TransformsInMeters(camera_to_world, meters_per_unit_),
      .focal_length = meters(GetFocalLength()),
      .aperture =
          {
              meters(GetHorizontalAperture()),
              meters(GetVerticalAperture()),
          },
      .aperture_offset =
          {
              meters(GetHorizontalApertureOffset()),
              meters(GetVerticalApertureOffset()),
          },
      .clipping_range = {meters(clipping.GetMin()), meters(clipping.GetMax())},
      .f_stop = static_cast<double>(GetFStop()),
      .focus_distance = meters(GetFocusDistance()),
      .exposure_scale = GetLinearExposureScale(),
  };
}

void CameraPrim::Place(const host::Shutter& shutter) {
  const SampledTransform camera_to_world =
      ResampleTransform(transform_samples_, GetTransform(), shutter);
  motion_outcome_ = camera_to_world.outcome;
  camera_ = Converted(camera_to_world.motion);
}

host::Camera CameraPrim::Sampled(const host::Shutter& shutter) const {
  return Converted(
      ResampleTransform(transform_samples_, GetTransform(), shutter).motion);
}

host::Camera CameraPrim::AtFrame() const {
  const std::array<pxr::GfMatrix4d, 1> at_frame{GetTransform()};
  return Converted(at_frame);
}

void CameraPrim::FreeHostCopy() {
  camera_ = {};
  changed_ = Changed::kNo;
}

}  // namespace umbra::delegate::internal
