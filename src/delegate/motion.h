// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Motion within the shutter (docs/adr/0012-umbra-is-a-hydra-renderer.md):
// the samples Hydra gives of a transform or of points, those within the
// shutter and those that bracket it, resampled to equally spaced times over
// the shutter, as the host copies hold them.

#ifndef UMBRA_DELEGATE_MOTION_H_
#define UMBRA_DELEGATE_MOTION_H_

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <vector>

#include "host_copy.h"
#include "motion_samples.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

namespace pxr = PXR_NS;

// The transform `weight` of the way from `start` to `end`, interpolated as
// scale (with shear), rotation and translation: the scale and translation
// linearly and the rotation along the shortest arc. Linear per element for a
// singular transform.
[[nodiscard]] pxr::GfMatrix4d BlendTransforms(const pxr::GfMatrix4d& start,
                                              const pxr::GfMatrix4d& end,
                                              double weight);

// Each transform of `start` blended with that of `end`.
[[nodiscard]] std::vector<pxr::GfMatrix4d> BlendTransformArrays(
    const std::vector<pxr::GfMatrix4d>& start,
    const std::vector<pxr::GfMatrix4d>& end, double weight);

// The points `weight` of the way from `start` to `end`, linearly; `start`
// and `end` are as long as each other.
[[nodiscard]] pxr::VtVec3fArray BlendPoints(const pxr::VtVec3fArray& start,
                                            const pxr::VtVec3fArray& end,
                                            double weight);

// A transform's motion samples, equally spaced over the shutter: one when
// it does not move.
using TransformMotion = std::vector<pxr::GfMatrix4d>;

// `inner` placed by `outer`, sample by sample, the one with fewer samples
// resampled to the count of the other.
[[nodiscard]] TransformMotion ComposeMotion(const TransformMotion& inner,
                                            const TransformMotion& outer);

// The samples Hydra gives of the transform of the prim at `prim_id` over
// `shutter`, which spans an interval; none when it has no transform.
[[nodiscard]] TimedSamples<pxr::GfMatrix4d> SampleTransform(
    pxr::HdSceneDelegate& delegate, const pxr::SdfPath& prim_id,
    const host::Shutter& shutter);

// The samples Hydra gives of the primvar `name` of the prim at `prim_id` over
// `shutter`, which spans an interval; none when it has no such primvar.
[[nodiscard]] TimedSamples<pxr::VtValue> SamplePrimvar(
    pxr::HdSceneDelegate& delegate, const pxr::SdfPath& prim_id,
    const pxr::TfToken& name, const host::Shutter& shutter);

// A transform's motion samples, and what sampling did.
struct SampledTransform {
  TransformMotion motion;
  MotionOutcome outcome = MotionOutcome::kSampled;
};

// The motion samples of a transform sampled at `samples` over `shutter`:
// `frame`, its value at the frame, alone when there is no motion blur or it
// does not move.
[[nodiscard]] SampledTransform ResampleTransform(
    const TimedSamples<pxr::GfMatrix4d>& samples, const pxr::GfMatrix4d& frame,
    const host::Shutter& shutter);

// The motion samples of the transform of the prim at `prim_id` over
// `shutter`: its transform at the frame alone when there is no motion blur or
// it does not move.
[[nodiscard]] SampledTransform SampleTransformMotion(
    pxr::HdSceneDelegate& delegate, const pxr::SdfPath& prim_id,
    const host::Shutter& shutter);

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_MOTION_H_
