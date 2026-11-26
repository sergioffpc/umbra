// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Instancing as Hydra 2.0 delivers it, native instances and PointInstancers
// alike: instancer prims, which place their prototypes, rprims or other
// instancers. The prototypes stay held once
// (docs/adr/0006-scenes-enter-through-hydra.md); nested instancers are
// composed into one level of instances of them.

#ifndef UMBRA_EXT_USD_INSTANCER_H_
#define UMBRA_EXT_USD_INSTANCER_H_

#include <pxr/base/gf/matrix4d.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "umbra/core/progress.h"
#include "umbra/core/safety.h"
#include "umbra/scene/scene.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd::internal {

namespace pxr = PXR_NS;

// An instance of a prototype, through every level of instancing above it.
struct FlatInstance {
  // The instance-to-world transform, in scene units, row-major, for row
  // vectors: those of the instancers, composed.
  pxr::GfMatrix4d transform{1.0};
  // Its own primvars, one value each: the innermost instancer's first.
  std::vector<scene::Primvar> primvars;
};

// An instancer prim, synced: its transform, those of its instances and
// their primvars. Hydra syncs it, under its own lock, before the first
// rprim it places reads it; its parents too.
class InstancerPrim final : public pxr::HdInstancer {
 public:
  // Counts its sync into `phase`.
  InstancerPrim(pxr::HdSceneDelegate* delegate, const pxr::SdfPath& prim_id,
                core::ProgressPhase& phase);

  void Sync(pxr::HdSceneDelegate* delegate,
            pxr::HdRenderParam* UMBRA_NULLABLE render_param,
            pxr::HdDirtyBits* dirty_bits) override;

  // The instances it places of `prototype`, an rprim or an instancer,
  // flattened through its parents: one for each instance of each parent.
  // None when it or a parent is invisible.
  [[nodiscard]] std::vector<FlatInstance> Instances(
      const pxr::SdfPath& prototype) const;

  // Whether Hydra synced it: whether an rprim it places was synced.
  [[nodiscard]] bool IsSynced() const { return synced_ == Synced::kSynced; }

 private:
  enum class Synced : std::uint8_t { kNotSynced, kSynced };
  enum class Visibility : std::uint8_t { kVisible, kInvisible };

  // The instance at `index`, of this level alone, in its parent's space.
  [[nodiscard]] FlatInstance Instance(std::size_t index) const;
  // The instances of this level alone, in its parent's space.
  [[nodiscard]] std::vector<FlatInstance> OwnInstances(
      const pxr::SdfPath& prototype) const;
  // The instancer that places it, or none at the top level.
  [[nodiscard]] const InstancerPrim* UMBRA_NULLABLE Parent() const;
  // The number of instances it places, of all its prototypes.
  [[nodiscard]] std::size_t CountInstances() const;

  core::ProgressPhase* phase_;
  Synced synced_ = Synced::kNotSynced;
  Visibility visibility_ = Visibility::kVisible;
  pxr::GfMatrix4d transform_{1.0};
  // The transform of each instance, in the instancer's space, by index.
  std::vector<pxr::GfMatrix4d> instance_transforms_;
  // The primvars given per instance, every instance's value in each.
  std::vector<scene::Primvar> primvars_;
};

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_INSTANCER_H_
