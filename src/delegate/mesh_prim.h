// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// A Mesh prim, as Hydra gives it, synced: its source read from the scene
// delegate, its points sampled over the shutter, converted into its host
// copy with its instances (mesh.h), and updated by what Hydra marks dirty.

#ifndef UMBRA_DELEGATE_MESH_PRIM_H_
#define UMBRA_DELEGATE_MESH_PRIM_H_

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "dicing.h"
#include "host_copy.h"
#include "light.h"
#include "mesh.h"
#include "motion_samples.h"
#include "sync_counter.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

namespace pxr = PXR_NS;

// What the sync of a mesh gave.
enum class MeshState : std::uint8_t {
  // It was not synced: its purpose is not rendered.
  kNotSynced,
  // It is invisible.
  kInvisible,
  // It is converted into its host copy, with its instances.
  kConverted,
  // It is invalid, and dropped.
  kInvalid,
};

// A GeomSubset of a mesh bound to a material.
struct MaterialSubset {
  pxr::SdfPath material;
  // The faces it holds: indices of the mesh's original polygons.
  std::vector<std::uint32_t> faces;
};

// Whether a mesh has a displayColor primvar.
enum class DisplayColor : std::uint8_t {
  kAbsent,
  kPresent,
};

// The dirty bit of a mesh whose MeshLightAPI changed, which Hydra has none
// of: the delegate maps a change of a mesh's light data source to it.
inline constexpr pxr::HdDirtyBits kDirtyEmission =
    pxr::HdChangeTracker::CustomBitsBegin;

// Has Hydra mark a mesh kDirtyEmission when its light data source changes:
// no dirty bit of Hydra's names it. The mapping is Hydra's, process-wide,
// for every mesh rprim, and comes on top of Hydra's own.
void MapEmissionToDirtyBit();

// A Mesh prim, synced into its host copy when it is visible and valid,
// with its instances: those its instancer places, flattened, or one of its
// own when it has no instancer. Implicit surfaces reach it as meshes. Its
// points are the output of its ext computation primvar `points` when it has
// one, as a skinned mesh does. Its points and its transform are sampled over
// the shutter, and its displayColor converted into ACEScg. Its material is
// the one bound to it, and its GeomSubsets bound to one keep theirs, each
// triangle given the index of its material: 0 for its own, i + 1 for that of
// its i-th subset. A mesh light's emission is read as a light's, and every
// instance emits it. A mesh that asks for subdivision is placed first, then
// refined for the dicing view on its closest instance (RefineMesh).
//
// A sync that finds it placed, visible with an instance or more, converts
// only what Hydra marked dirty, as change flags: its transform or its
// instancer's give kPlacement; its points kPoints, alone, or kPrimvars too
// when its normals are computed from them; its primvars or its normals
// kPrimvars, and kMaterials too when it gains or loses a displayColor; its
// material kMaterials; its topology or its sidedness kTopology, with
// kPrimvars and kMaterials; and its MeshLightAPI kEmission. A subdivided mesh
// is refined again when its topology, its subdivision tags, its points, its
// primvars, its placement or its sidedness are dirty, or when the delegate
// has it refined for a new dicing view, with kTopology, kPrimvars and
// kMaterials, and kPlacement when its placement changed. Its host copy then
// holds only the members those flags name. Any other sync converts all of it.
class MeshPrim final : public pxr::HdMesh {
 public:
  // Counts its sync into `counter`; its lengths are scaled by
  // `meters_per_unit`. `shutter` and `dicing` are chosen before it syncs.
  MeshPrim(const pxr::SdfPath& prim_id, SyncCounter& counter,
           double meters_per_unit,
           const host::Shutter& shutter UMBRA_LIFETIMEBOUND,
           const DicingView& dicing UMBRA_LIFETIMEBOUND);

  [[nodiscard]] pxr::HdDirtyBits GetInitialDirtyBitsMask() const override;
  void Sync(pxr::HdSceneDelegate* delegate,
            pxr::HdRenderParam* UMBRA_NULLABLE render_param,
            pxr::HdDirtyBits* dirty_bits, const pxr::TfToken& repr) override;

  [[nodiscard]] MeshState state() const { return state_; }
  // Whether it is converted, and refined from a subdivision surface.
  [[nodiscard]] bool IsRefined() const {
    return state_ == MeshState::kConverted && !subdivision_scheme_.empty();
  }
  // The subdivisionScheme it asks for, once synced: empty for none.
  [[nodiscard]] const std::string& subdivision_scheme() const
      UMBRA_LIFETIMEBOUND {
    return subdivision_scheme_;
  }
  // Why it is invalid.
  [[nodiscard]] const std::string& invalid_reason() const UMBRA_LIFETIMEBOUND {
    return invalid_reason_;
  }
  // What sampling did with its motion, once converted.
  [[nodiscard]] MotionOutcome motion_outcome() const {
    return std::max(points_outcome_, placement_outcome_);
  }
  // What its MeshLightAPI authors that umbra does not take, as of its last
  // reading of its emission.
  [[nodiscard]] const std::vector<LightNote>& emission_notes() const
      UMBRA_LIFETIMEBOUND {
    return emission_notes_;
  }
  // Whether it has a displayColor, once converted.
  [[nodiscard]] DisplayColor display_color() const { return display_color_; }
  // What its host copy holds since it was last committed: host::kWholeMesh
  // when a sync converted all of it, the members of its updates otherwise,
  // and kNone when it did not sync since.
  [[nodiscard]] device::ChangeFlags changes() const { return changes_; }
  // Its mesh, once converted: the host copy, whose materials the commit
  // gives.
  [[nodiscard]] host::Mesh& mesh() UMBRA_LIFETIMEBOUND { return mesh_; }
  // Its GeomSubsets bound to a material, once converted.
  [[nodiscard]] const std::vector<MaterialSubset>& material_subsets() const
      UMBRA_LIFETIMEBOUND {
    return material_subsets_;
  }
  // Its instances, once placed: their object-to-world transforms in meters,
  // one per motion sample, and their own primvars. None when its instancer
  // places none.
  [[nodiscard]] const std::vector<host::Instance>& instances() const
      UMBRA_LIFETIMEBOUND {
    return instances_;
  }
  // The instances it placed when it was last placed, which its host copy no
  // longer holds once committed.
  [[nodiscard]] std::size_t instance_count() const { return instance_count_; }
  // Frees its host copy, once committed.
  void FreeHostCopy();
  // Refines it again for the dicing view, which changed, through the scene
  // delegate it last synced from: when it is refined and placed, and Hydra
  // will not sync it for a change that refines it anyway. Hydra syncs the
  // rprims in parallel only after the tasks, which choose the view; meshes
  // may refine in parallel, each its own.
  void RefineForView();

 protected:
  // NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): the
  // names of the methods OpenUSD has rprims override.
  [[nodiscard]] pxr::HdDirtyBits _PropagateDirtyBits(
      pxr::HdDirtyBits bits) const override;
  void _InitRepr(const pxr::TfToken& repr,
                 pxr::HdDirtyBits* dirty_bits) override;
  // NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

 private:
  // Logs its sync at trace level: with its size when converted whole.
  void LogSync() const;
  // Logs its refinement at debug level: its triangles and the longest edge.
  void LogRefined() const;
  // Converts all of it.
  void Convert(pxr::HdSceneDelegate& delegate);
  // Converts what `bits` mark dirty, once placed.
  void Update(pxr::HdSceneDelegate& delegate, pxr::HdDirtyBits bits);
  // Refines it again, placed again, once placed: `bits` tell what else
  // changed.
  void Refine(pxr::HdSceneDelegate& delegate, pxr::HdDirtyBits bits);
  // The flags of an update of what `bits` mark dirty, but its placement,
  // the members they name converted; none when it turns out invalid.
  // The members of the host copy that `bits` dirty: kPrimvars with
  // kPoints when its normals are computed.
  [[nodiscard]] device::ChangeFlags DirtiedOf(pxr::HdDirtyBits bits) const;
  // Converts its geometry again, `dirtied` dirty, and gives what changed:
  // none when it no longer converts.
  std::optional<device::ChangeFlags> ReconvertGeometry(
      pxr::HdSceneDelegate& delegate, device::ChangeFlags dirtied);
  std::optional<device::ChangeFlags> UpdateGeometry(
      pxr::HdSceneDelegate& delegate, pxr::HdDirtyBits bits);
  // What converting its points alone gave.
  enum class PointsConversion : std::uint8_t {
    kConverted,
    // Their count, or the outcome of their sampling, changed: all of it
    // must be converted.
    kWholeNeeded,
  };

  // Converts its geometry, its primvars and the materials of its triangles:
  // it is then converted, or invalid. A subdivided mesh is placed first.
  void ConvertGeometry(pxr::HdSceneDelegate& delegate);
  // The mesh as the scene delegate gives it, its points sampled over the
  // shutter; records whether they move.
  SourceMesh ReadSource(pxr::HdSceneDelegate& delegate);
  // Converts the display color into the mesh's primvars and records
  // whether it has one.
  void ReadDisplayColor(pxr::HdSceneDelegate& delegate);
  // Converts its points alone.
  PointsConversion ConvertPointsAlone(pxr::HdSceneDelegate& delegate);
  // Gives each triangle of its host copy the index of its material, from
  // its subsets.
  void AssignTriangleMaterials();
  // Its instances, from its transform and its instancer's, over the shutter.
  void Place(pxr::HdSceneDelegate& delegate);
  // Frees the members of its host copy that its changes do not name.
  void KeepChanged();
  // Reads its emission into its host copy, and its notes.
  void ReadEmission(pxr::HdSceneDelegate& delegate);

  SyncCounter* counter_;
  double meters_per_unit_;
  const host::Shutter* shutter_;
  const DicingView* dicing_;
  // The scene delegate of its last sync.
  pxr::HdSceneDelegate* UMBRA_NULLABLE delegate_ = nullptr;
  MeshState state_ = MeshState::kNotSynced;
  MotionOutcome points_outcome_ = MotionOutcome::kSampled;
  MotionOutcome placement_outcome_ = MotionOutcome::kSampled;
  NormalsSource normals_ = NormalsSource::kAuthored;
  DisplayColor display_color_ = DisplayColor::kAbsent;
  std::size_t point_count_ = 0;
  std::size_t instance_count_ = 0;
  // The longest edge of its triangles in pixels, when refined.
  double longest_edge_ = 0.0;
  device::ChangeFlags changes_ = device::ChangeFlags::kNone;
  std::string subdivision_scheme_;
  std::string invalid_reason_;
  host::Mesh mesh_;
  std::vector<MaterialSubset> material_subsets_;
  std::vector<host::Instance> instances_;
  std::vector<LightNote> emission_notes_;
};

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_MESH_PRIM_H_
