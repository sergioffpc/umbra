// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "motion.h"

#include <pxr/base/gf/matrix3d.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/timeSampleArray.h>
#include <pxr/usd/sdf/path.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

#include "host_copy.h"
#include "motion_samples.h"

namespace umbra::delegate::internal {
namespace {

// A transform as scale (with shear), rotation and translation, applied in
// that order to row vectors.
struct Decomposed {
  pxr::GfMatrix3d stretch;
  pxr::GfQuatd rotation;
  pxr::GfVec3d translation;
};

// `transform` decomposed, or none when it is singular.
std::optional<Decomposed> Decompose(const pxr::GfMatrix4d& transform) {
  pxr::GfMatrix4d scale_orientation(1.0);
  pxr::GfVec3d scale(0.0);
  pxr::GfMatrix4d rotation(1.0);
  pxr::GfVec3d translation(0.0);
  pxr::GfMatrix4d projection(1.0);
  if (!transform.Factor(&scale_orientation, &scale, &rotation, &translation,
                        &projection)) {
    return std::nullopt;
  }
  pxr::GfMatrix3d proper = rotation.ExtractRotationMatrix();
  // A mirroring transform leaves an improper rotation: its mirror goes into
  // the stretch instead.
  if (proper.GetDeterminant() < 0.0) {
    proper *= -1.0;
  }
  return Decomposed{
      .stretch = transform.ExtractRotationMatrix() * proper.GetTranspose(),
      .rotation = proper.ExtractRotation().GetQuat(),
      .translation = translation,
  };
}

pxr::GfMatrix4d Recompose(const Decomposed& parts) {
  pxr::GfMatrix3d rotation(1.0);
  rotation.SetRotate(parts.rotation);
  pxr::GfMatrix4d transform(1.0);
  transform.SetTransform(parts.stretch * rotation, parts.translation);
  return transform;
}

// Equally spaced samples, from 0 to 1, as timed samples.
TimedSamples<pxr::GfMatrix4d> Unit(const TransformMotion& motion) {
  TimedSamples<pxr::GfMatrix4d> samples;
  const std::size_t count = motion.size();
  for (std::size_t index = 0; index < count; ++index) {
    samples.times.push_back(count == 1 ? 0.0F
                                       : static_cast<float>(index) /
                                             static_cast<float>(count - 1));
  }
  samples.values = motion;
  return samples;
}

// `motion` resampled to `count` equally spaced samples.
TransformMotion ResampleUniform(const TransformMotion& motion,
                                std::size_t count) {
  if (motion.size() == count) {
    return motion;
  }
  return Resample(Unit(motion), host::Shutter{.open = 0.0, .close = 1.0}, count,
                  BlendTransforms);
}

// The samples of a Hydra time sample array.
template <typename T>
TimedSamples<T> FromArray(
    const pxr::HdTimeSampleArray<T, kMaxMotionSamples>& sampled) {
  TimedSamples<T> samples;
  samples.times.reserve(sampled.count);
  samples.values.reserve(sampled.count);
  for (const auto& [time, value] :
       std::views::zip(sampled.times, sampled.values) |
           std::views::take(sampled.count)) {
    samples.times.push_back(time);
    samples.values.push_back(value);
  }
  return samples;
}

}  // namespace

pxr::GfMatrix4d BlendTransforms(const pxr::GfMatrix4d& start,
                                const pxr::GfMatrix4d& end, double weight) {
  if (start == end) {
    return start;
  }
  const std::optional<Decomposed> first = Decompose(start);
  const std::optional<Decomposed> last = Decompose(end);
  if (!first.has_value() || !last.has_value()) {
    return (start * (1.0 - weight)) + (end * weight);
  }
  // The shortest arc: q and -q are the same rotation.
  const pxr::GfQuatd last_rotation =
      pxr::GfDot(first->rotation, last->rotation) < 0.0 ? last->rotation * -1.0
                                                        : last->rotation;
  return Recompose({
      .stretch = (first->stretch * (1.0 - weight)) + (last->stretch * weight),
      .rotation = pxr::GfSlerp(weight, first->rotation, last_rotation),
      .translation =
          (first->translation * (1.0 - weight)) + (last->translation * weight),
  });
}

std::vector<pxr::GfMatrix4d> BlendTransformArrays(
    const std::vector<pxr::GfMatrix4d>& start,
    const std::vector<pxr::GfMatrix4d>& end, double weight) {
  std::vector<pxr::GfMatrix4d> blended;
  const std::size_t count = std::min(start.size(), end.size());
  blended.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    blended.push_back(BlendTransforms(start.at(index), end.at(index), weight));
  }
  return blended;
}

pxr::VtVec3fArray BlendPoints(const pxr::VtVec3fArray& start,
                              const pxr::VtVec3fArray& end, double weight) {
  const auto end_weight = static_cast<float>(weight);
  const float start_weight = 1.0F - end_weight;
  pxr::VtVec3fArray blended;
  blended.reserve(std::min(start.size(), end.size()));
  for (const auto& [first, last] : std::views::zip(start, end)) {
    // GfVec3f scales by doubles: the components are blended as floats.
    blended.push_back(
        pxr::GfVec3f((first.x * start_weight) + (last.x * end_weight),
                     (first.y * start_weight) + (last.y * end_weight),
                     (first.z * start_weight) + (last.z * end_weight)));
  }
  return blended;
}

TransformMotion ComposeMotion(const TransformMotion& inner,
                              const TransformMotion& outer) {
  const std::size_t count = std::max(inner.size(), outer.size());
  const TransformMotion inner_samples = ResampleUniform(inner, count);
  const TransformMotion outer_samples = ResampleUniform(outer, count);
  TransformMotion composed;
  composed.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    composed.push_back(inner_samples.at(index) * outer_samples.at(index));
  }
  return composed;
}

TimedSamples<pxr::GfMatrix4d> SampleTransform(pxr::HdSceneDelegate& delegate,
                                              const pxr::SdfPath& prim_id,
                                              const host::Shutter& shutter) {
  pxr::HdTimeSampleArray<pxr::GfMatrix4d, kMaxMotionSamples> sampled;
  delegate.SampleTransform(prim_id, static_cast<float>(shutter.open),
                           static_cast<float>(shutter.close), &sampled);
  return FromArray(sampled);
}

TimedSamples<pxr::VtValue> SamplePrimvar(pxr::HdSceneDelegate& delegate,
                                         const pxr::SdfPath& prim_id,
                                         const pxr::TfToken& name,
                                         const host::Shutter& shutter) {
  pxr::HdTimeSampleArray<pxr::VtValue, kMaxMotionSamples> sampled;
  delegate.SamplePrimvar(prim_id, name, static_cast<float>(shutter.open),
                         static_cast<float>(shutter.close), &sampled);
  return FromArray(sampled);
}

SampledTransform ResampleTransform(const TimedSamples<pxr::GfMatrix4d>& samples,
                                   const pxr::GfMatrix4d& frame,
                                   const host::Shutter& shutter) {
  Motion<pxr::GfMatrix4d> motion = MotionOf(samples, shutter, BlendTransforms);
  if (motion.samples.empty()) {
    return {.motion = {frame}, .outcome = MotionOutcome::kSampled};
  }
  return {.motion = std::move(motion.samples), .outcome = motion.outcome};
}

SampledTransform SampleTransformMotion(pxr::HdSceneDelegate& delegate,
                                       const pxr::SdfPath& prim_id,
                                       const host::Shutter& shutter) {
  const pxr::GfMatrix4d frame = delegate.GetTransform(prim_id);
  if (!IsMotionBlurred(shutter)) {
    return {.motion = {frame}, .outcome = MotionOutcome::kSampled};
  }
  return ResampleTransform(SampleTransform(delegate, prim_id, shutter), frame,
                           shutter);
}

}  // namespace umbra::delegate::internal
