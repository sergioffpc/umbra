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
#include <cstddef>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "mesh.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/core/progress.h"
#include "umbra/core/safety.h"
#include "umbra/scene/scene.h"

namespace umbra::ext::usd::internal {
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

// The transform of each instance of the instancer at `prim_id`, from its
// hydra:instance* primvars, in their order: transform, scale, rotation,
// then translation. An instance an array is too short for has the identity
// in it.
std::vector<Matrix> InstanceTransforms(pxr::HdSceneDelegate& delegate,
                                       const pxr::SdfPath& prim_id) {
  const auto get = [&](const pxr::TfToken& name) {
    return delegate.Get(prim_id, name);
  };
  const std::vector<std::vector<Matrix>> parts = {
      Matrices<pxr::GfMatrix4d, pxr::GfMatrix4f>(
          get(pxr::HdInstancerTokens->instanceTransforms),
          [](const auto& matrix) { return Matrix(matrix); }),
      Matrices<pxr::GfVec3f, pxr::GfVec3d, pxr::GfVec3h>(
          get(pxr::HdInstancerTokens->instanceScales),
          [](const auto& scale) { return Scale(pxr::GfVec3d(scale)); }),
      Matrices<pxr::GfQuath, pxr::GfQuatf, pxr::GfQuatd>(
          get(pxr::HdInstancerTokens->instanceRotations),
          [](const auto& rotation) {
            return Rotation(pxr::GfQuatd(rotation));
          }),
      Matrices<pxr::GfVec3f, pxr::GfVec3d, pxr::GfVec3h>(
          get(pxr::HdInstancerTokens->instanceTranslations),
          [](const auto& translation) {
            return Translation(pxr::GfVec3d(translation));
          }),
  };
  std::size_t count = 0;
  for (const std::vector<Matrix>& part : parts) {
    count = std::max(count, part.size());
  }
  std::vector<Matrix> transforms(count, Matrix(1.0));
  for (const std::vector<Matrix>& part : parts) {
    for (std::size_t i = 0; i < part.size(); ++i) {
      transforms.at(i) *= part.at(i);
    }
  }
  return transforms;
}

// Whether `name` is one of the hydra:instance* primvars, which place the
// instances rather than shade them.
bool IsPlacement(const pxr::TfToken& name) {
  return name == pxr::HdInstancerTokens->instanceTransforms ||
         name == pxr::HdInstancerTokens->instanceScales ||
         name == pxr::HdInstancerTokens->instanceRotations ||
         name == pxr::HdInstancerTokens->instanceTranslations;
}

// The numeric primvars the instancer at `prim_id` gives per instance; the
// others are dropped with a debug message.
std::vector<scene::Primvar> InstancePrimvars(pxr::HdSceneDelegate& delegate,
                                             const pxr::SdfPath& prim_id) {
  std::vector<scene::Primvar> primvars;
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
  return primvars;
}

// The value at `index` of `primvar`, as a primvar of one value.
scene::Primvar ValueAt(const scene::Primvar& primvar, std::size_t index) {
  scene::Primvar element{
      .name = primvar.name,
      .rate = scene::PrimvarRate::kConstant,
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
bool HasPrimvar(const std::vector<scene::Primvar>& primvars,
                const std::string& name) {
  return std::ranges::any_of(primvars, [&](const scene::Primvar& primvar) {
    return primvar.name == name;
  });
}

// `inner` placed by `outer`: their transforms composed, and the primvars of
// `outer` that `inner` does not give.
FlatInstance Compose(const FlatInstance& inner, const FlatInstance& outer) {
  FlatInstance composed{
      .transform = inner.transform * outer.transform,
      .primvars = inner.primvars,
  };
  for (const scene::Primvar& primvar : outer.primvars) {
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
                             const pxr::SdfPath& prim_id,
                             core::ProgressPhase& phase)
    : pxr::HdInstancer(delegate, prim_id), phase_(&phase) {}

void InstancerPrim::Sync(pxr::HdSceneDelegate* delegate,
                         pxr::HdRenderParam* UMBRA_NULLABLE /*render_param*/,
                         pxr::HdDirtyBits* dirty_bits) {
  _UpdateInstancer(delegate, dirty_bits);
  visibility_ = delegate->GetVisible(GetId()) ? Visibility::kVisible
                                              : Visibility::kInvisible;
  transform_ = delegate->GetInstancerTransform(GetId());
  instance_transforms_ = InstanceTransforms(*delegate, GetId());
  primvars_ = InstancePrimvars(*delegate, GetId());
  if (synced_ == Synced::kNotSynced) {
    synced_ = Synced::kSynced;
    phase_->Add(1);
    const std::size_t instances = CountInstances();
    UMBRA_LOG_TRACE(kUsd, "synced {} ({} instance{})", GetId().GetString(),
                    core::FormatCount(instances), instances == 1 ? "" : "s");
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
  instance.transform = index < instance_transforms_.size()
                           ? instance_transforms_.at(index) * transform_
                           : transform_;
  for (const scene::Primvar& primvar : primvars_) {
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

}  // namespace umbra::ext::usd::internal
