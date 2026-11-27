// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "instancer.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/quath.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/enums.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/usd/sdf/path.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "host_copy.h"
#include "material.h"
#include "mesh.h"
#include "motion.h"
#include "motion_samples.h"
#include "sync_counter.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/core/safety.h"

namespace umbra::delegate::internal {
namespace {

using Matrix = pxr::GfMatrix4d;

Matrix Translation(const pxr::GfVec3d& translation) {
  Matrix matrix(1.0);
  matrix.SetTranslate(translation);
  return matrix;
}

Matrix Rotation(const pxr::GfQuatd& rotation) {
  Matrix matrix(1.0);
  matrix.SetRotate(rotation);
  return matrix;
}

Matrix Scale(const pxr::GfVec3d& scale) {
  Matrix matrix(1.0);
  matrix.SetScale(scale);
  return matrix;
}

// The elements of `value`, an array of one of Ts, each made a matrix by
// `make`; none for a value of another type.
template <typename... Ts, typename Make>
std::vector<Matrix> Matrices(const pxr::VtValue& value, const Make& make) {
  std::vector<Matrix> matrices;
  const auto take = [&]<typename T> {
    if (!value.IsHolding<pxr::VtArray<T>>()) {
      return false;
    }
    for (const T& element : value.UncheckedGet<pxr::VtArray<T>>()) {
      matrices.push_back(make(element));
    }
    return true;
  };
  static_cast<void>((take.template operator()<Ts>() || ...));
  return matrices;
}

// The hydra:instance* primvars that place the instances, in the order they
// apply: transform, scale, rotation, then translation.
enum class Part : std::uint8_t {
  kTransforms,
  kScales,
  kRotations,
  kTranslations,
};
constexpr std::array kParts = {
    Part::kTransforms,
    Part::kScales,
    Part::kRotations,
    Part::kTranslations,
};

// One part of the placement of each instance, as a transform.
using PartTransforms = std::vector<Matrix>;
// The samples of each part, in the order of kParts.
using PartSamples = std::array<TimedSamples<PartTransforms>, kParts.size()>;

// The name of the primvar of `part`.
const pxr::TfToken& NameOf(Part part) {
  switch (part) {
    case Part::kTransforms:
      return pxr::HdInstancerTokens->instanceTransforms;
    case Part::kScales:
      return pxr::HdInstancerTokens->instanceScales;
    case Part::kRotations:
      return pxr::HdInstancerTokens->instanceRotations;
    case Part::kTranslations:
      break;
  }
  return pxr::HdInstancerTokens->instanceTranslations;
}

// `value`, the primvar of `part`, as a transform per instance; none for a
// value of another type.
PartTransforms TransformsOf(Part part, const pxr::VtValue& value) {
  switch (part) {
    case Part::kTransforms:
      return Matrices<pxr::GfMatrix4d, pxr::GfMatrix4f>(
          value, [](const auto& matrix) { return Matrix(matrix); });
    case Part::kScales:
      return Matrices<pxr::GfVec3f, pxr::GfVec3d, pxr::GfVec3h>(
          value, [](const auto& scale) { return Scale(pxr::GfVec3d(scale)); });
    case Part::kRotations:
      return Matrices<pxr::GfQuath, pxr::GfQuatf, pxr::GfQuatd>(
          value, [](const auto& rotation) {
            return Rotation(pxr::GfQuatd(rotation));
          });
    case Part::kTranslations:
      break;
  }
  return Matrices<pxr::GfVec3f, pxr::GfVec3d, pxr::GfVec3h>(
      value, [](const auto& translation) {
        return Translation(pxr::GfVec3d(translation));
      });
}

// The samples of `part` of the instancer at `prim_id` over `shutter`: its
// value at the frame alone when the shutter spans no interval.
TimedSamples<PartTransforms> SamplePart(pxr::HdSceneDelegate& delegate,
                                        const pxr::SdfPath& prim_id, Part part,
                                        const host::Shutter& shutter) {
  TimedSamples<pxr::VtValue> values;
  if (IsMotionBlurred(shutter)) {
    values = SamplePrimvar(delegate, prim_id, NameOf(part), shutter);
  }
  if (values.values.empty()) {
    values = {.times = {0.0F}, .values = {delegate.Get(prim_id, NameOf(part))}};
  }
  TimedSamples<PartTransforms> samples{
      .times = std::move(values.times),
      .values = {},
  };
  samples.values.reserve(values.values.size());
  for (const pxr::VtValue& value : values.values) {
    samples.values.push_back(TransformsOf(part, value));
  }
  return samples;
}

PartSamples SampleParts(pxr::HdSceneDelegate& delegate,
                        const pxr::SdfPath& prim_id,
                        const host::Shutter& shutter) {
  PartSamples samples;
  for (const Part part : kParts) {
    samples.at(static_cast<std::size_t>(part)) =
        SamplePart(delegate, prim_id, part, shutter);
  }
  return samples;
}

// Whether the number of instances some part places changes among its
// samples.
bool HasChangingCount(const PartSamples& samples) {
  return std::ranges::any_of(
      samples, [](const auto& part) { return !HasSameSizes(part.values); });
}

// The transform of each instance, from the parts of its placement at one
// time. An instance a part is too short for has the identity in it.
std::vector<Matrix> InstanceTransforms(
    const std::array<PartTransforms, kParts.size()>& parts) {
  std::size_t count = 0;
  for (const PartTransforms& part : parts) {
    count = std::max(count, part.size());
  }
  std::vector<Matrix> transforms(count, Matrix(1.0));
  for (const PartTransforms& part : parts) {
    for (std::size_t i = 0; i < part.size(); ++i) {
      transforms.at(i) *= part.at(i);
    }
  }
  return transforms;
}

// The transform of each instance at `time`.
std::vector<Matrix> InstanceTransformsAt(const PartSamples& samples,
                                         double time) {
  std::array<PartTransforms, kParts.size()> parts;
  for (std::size_t part = 0; part < kParts.size(); ++part) {
    parts.at(part) = ValueAt(samples.at(part), time, BlendTransformArrays);
  }
  return InstanceTransforms(parts);
}

// The motion samples of each instance's transform: `count` equally spaced
// over `shutter`, or one when it does not move.
std::vector<TransformMotion> InstanceMotion(const PartSamples& samples,
                                            const host::Shutter& shutter,
                                            std::size_t count) {
  std::vector<TransformMotion> motion;
  for (std::size_t index = 0; index < count; ++index) {
    const std::vector<Matrix> transforms =
        InstanceTransformsAt(samples, UniformTime(shutter, index, count));
    motion.resize(std::max(motion.size(), transforms.size()));
    for (std::size_t instance = 0; instance < transforms.size(); ++instance) {
      motion.at(instance).push_back(transforms.at(instance));
    }
  }
  for (TransformMotion& instance : motion) {
    CollapseStill(instance);
  }
  return motion;
}

// Whether `name` is one of the hydra:instance* primvars, which place the
// instances rather than shade them.
bool IsPlacement(const pxr::TfToken& name) {
  return name == pxr::HdInstancerTokens->instanceTransforms ||
         name == pxr::HdInstancerTokens->instanceScales ||
         name == pxr::HdInstancerTokens->instanceRotations ||
         name == pxr::HdInstancerTokens->instanceTranslations;
}

// The numeric primvars the instancer at `prim_id` gives per instance, its
// displayColor in ACEScg; the others are dropped with a debug message.
std::vector<host::Primvar> InstancePrimvars(pxr::HdSceneDelegate& delegate,
                                            const pxr::SdfPath& prim_id) {
  std::vector<host::Primvar> primvars;
  for (const pxr::HdPrimvarDescriptor& descriptor :
       delegate.GetPrimvarDescriptors(prim_id, pxr::HdInterpolationInstance)) {
    if (IsPlacement(descriptor.name)) {
      continue;
    }
    const pxr::VtValue value = delegate.Get(prim_id, descriptor.name);
    auto primvar = ConstantPrimvar(descriptor.name.GetString(), value);
    if (!primvar.has_value()) {
      UMBRA_LOG_DEBUG(kUsd, "{}: primvar {} of type {} not supported; dropped",
                      prim_id.GetString(), descriptor.name.GetString(),
                      value.GetTypeName());
      continue;
    }
    primvars.push_back(std::move(*primvar));
  }
  ConvertDisplayColor(delegate, prim_id, primvars);
  return primvars;
}

// The value at `index` of `primvar`, as a primvar of one value.
host::Primvar ValueAt(const host::Primvar& primvar, std::size_t index) {
  host::Primvar element{
      .name = primvar.name,
      .rate = host::PrimvarRate::kConstant,
      .components = primvar.components,
      .values = {},
      .indices = {},
  };
  std::visit(
      [&](const auto& numbers) {
        const auto first = std::span(numbers).subspan(
            index * primvar.components, primvar.components);
        element.values.template emplace<std::remove_cvref_t<decltype(numbers)>>(
            first.begin(), first.end());
      },
      primvar.values);
  return element;
}

// Whether `primvars` holds one named `name`.
bool HasPrimvar(const std::vector<host::Primvar>& primvars,
                const std::string& name) {
  return std::ranges::any_of(primvars, [&](const host::Primvar& primvar) {
    return primvar.name == name;
  });
}

// `inner` placed by `outer`: their transforms composed, and the primvars of
// `outer` that `inner` does not give.
FlatInstance Compose(const FlatInstance& inner, const FlatInstance& outer) {
  FlatInstance composed{
      .transforms = ComposeMotion(inner.transforms, outer.transforms),
      .primvars = inner.primvars,
  };
  for (const host::Primvar& primvar : outer.primvars) {
    if (!HasPrimvar(composed.primvars, primvar.name)) {
      composed.primvars.push_back(primvar);
    }
  }
  return composed;
}

// Every instance of `inner` placed by each of `outer`.
std::vector<FlatInstance> Flatten(const std::vector<FlatInstance>& inner,
                                  const std::vector<FlatInstance>& outer) {
  std::vector<FlatInstance> flattened;
  flattened.reserve(outer.size() * inner.size());
  for (const FlatInstance& placement : outer) {
    for (const FlatInstance& instance : inner) {
      flattened.push_back(Compose(instance, placement));
    }
  }
  return flattened;
}

}  // namespace

InstancerPrim::InstancerPrim(pxr::HdSceneDelegate* delegate,
                             const pxr::SdfPath& prim_id, SyncCounter& counter,
                             const host::Shutter& shutter)
    : pxr::HdInstancer(delegate, prim_id),
      counter_(&counter),
      shutter_(&shutter) {}

void InstancerPrim::Sync(pxr::HdSceneDelegate* delegate,
                         pxr::HdRenderParam* UMBRA_NULLABLE /*render_param*/,
                         pxr::HdDirtyBits* dirty_bits) {
  _UpdateInstancer(delegate, dirty_bits);
  visibility_ = delegate->GetVisible(GetId()) ? Visibility::kVisible
                                              : Visibility::kInvisible;
  SampleMotion(*delegate);
  primvars_ = InstancePrimvars(*delegate, GetId());
  if (synced_ == Synced::kNotSynced) {
    synced_ = Synced::kSynced;
    counter_->Add(1);
    const std::size_t instances = CountInstances();
    UMBRA_LOG_TRACE(kUsd, "synced {} ({} instance{})", GetId().GetString(),
                    core::FormatCount(instances), instances == 1 ? "" : "s");
  }
}

void InstancerPrim::SampleMotion(pxr::HdSceneDelegate& delegate) {
  host::Shutter shutter = *shutter_;
  motion_outcome_ = MotionOutcome::kSampled;
  PartSamples parts = SampleParts(delegate, GetId(), shutter);
  // Instances that appear or vanish within the shutter have no blur, nor
  // has any other of the instancer's.
  if (HasChangingCount(parts)) {
    motion_outcome_ = MotionOutcome::kTopologyChanges;
    shutter = {};
    parts = SampleParts(delegate, GetId(), shutter);
  }
  const pxr::GfMatrix4d frame = delegate.GetInstancerTransform(GetId());
  TimedSamples<pxr::GfMatrix4d> transform{.times = {0.0F}, .values = {frame}};
  if (IsMotionBlurred(shutter)) {
    transform = SampleTransform(delegate, GetId(), shutter);
  }
  MotionSampleCount count = CountMotionSamples(transform.times, shutter);
  for (const auto& part : parts) {
    count = Merge(count, CountMotionSamples(part.times, shutter));
  }
  transform_ = ResampleTransform(transform, frame, shutter).motion;
  instance_transforms_ = InstanceMotion(parts, shutter, count.count);
  const bool moves =
      transform_.size() > 1 ||
      std::ranges::any_of(instance_transforms_, [](const auto& instance) {
        return instance.size() > 1;
      });
  if (motion_outcome_ == MotionOutcome::kSampled && moves) {
    motion_outcome_ = count.outcome;
  }
}

std::size_t InstancerPrim::CountInstances() const {
  std::size_t count = 0;
  for (const pxr::SdfPath& prototype :
       GetDelegate()->GetInstancerPrototypes(GetId())) {
    count += GetDelegate()->GetInstanceIndices(GetId(), prototype).size();
  }
  return count;
}

FlatInstance InstancerPrim::Instance(std::size_t index) const {
  FlatInstance instance;
  instance.transforms =
      index < instance_transforms_.size()
          ? ComposeMotion(instance_transforms_.at(index), transform_)
          : transform_;
  for (const host::Primvar& primvar : primvars_) {
    if (index < primvar.Count()) {
      instance.primvars.push_back(ValueAt(primvar, index));
    }
  }
  return instance;
}

std::vector<FlatInstance> InstancerPrim::OwnInstances(
    const pxr::SdfPath& prototype) const {
  std::vector<FlatInstance> instances;
  if (visibility_ == Visibility::kInvisible) {
    return instances;
  }
  const pxr::VtIntArray indices =
      GetDelegate()->GetInstanceIndices(GetId(), prototype);
  instances.reserve(indices.size());
  for (const int index : indices) {
    // A negative index names no instance.
    if (index >= 0) {
      instances.push_back(Instance(static_cast<std::size_t>(index)));
    }
  }
  return instances;
}

const InstancerPrim* UMBRA_NULLABLE InstancerPrim::Parent() const {
  if (GetParentId().IsEmpty()) {
    return nullptr;
  }
  return dynamic_cast<const InstancerPrim*>(
      GetDelegate()->GetRenderIndex().GetInstancer(GetParentId()));
}

std::vector<FlatInstance> InstancerPrim::Instances(
    const pxr::SdfPath& prototype) const {
  std::vector<FlatInstance> instances = OwnInstances(prototype);
  // Each parent places every instance of the level below once for each of
  // its own instances of that level's instancer.
  const InstancerPrim* level = this;
  for (const InstancerPrim* parent = Parent();
       parent != nullptr && !instances.empty();
       level = parent, parent = parent->Parent()) {
    const std::vector<FlatInstance> outer =
        parent->OwnInstances(level->GetId());
    instances = Flatten(instances, outer);
  }
  return instances;
}

Placement PlaceInstances(pxr::HdSceneDelegate& delegate,
                         const pxr::SdfPath& prim_id,
                         const pxr::SdfPath& instancer_id,
                         const host::Shutter& shutter) {
  const SampledTransform prim_to_instance =
      SampleTransformMotion(delegate, prim_id, shutter);
  Placement placement{.instances = {}, .outcome = prim_to_instance.outcome};
  if (instancer_id.IsEmpty()) {
    placement.instances.emplace_back();
  } else {
    pxr::HdRenderIndex& index = delegate.GetRenderIndex();
    pxr::HdInstancer::_SyncInstancerAndParents(index, instancer_id);
    // Instancers are all umbra's: the delegate creates no other.
    if (const auto* instancer = dynamic_cast<const InstancerPrim*>(
            index.GetInstancer(instancer_id));
        instancer != nullptr) {
      placement.instances = instancer->Instances(prim_id);
    }
  }
  for (FlatInstance& instance : placement.instances) {
    instance.transforms =
        ComposeMotion(prim_to_instance.motion, instance.transforms);
    CollapseStill(instance.transforms);
  }
  return placement;
}

}  // namespace umbra::delegate::internal
