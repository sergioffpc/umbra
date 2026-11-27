// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// umbra's side of Hydra (docs/adr/0012-umbra-is-a-hydra-renderer.md): the
// render delegate, created directly rather than as a plugin, its prims, which
// convert what they sync into their host copies, and the task that runs in
// HdEngine::Execute and only syncs. Hydra syncs rprims in parallel, each into
// its own prim. The delegate's CommitResources commits the host copies to the
// devices, then frees them
// (docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_EXT_USD_HYDRA_H_
#define UMBRA_EXT_USD_HYDRA_H_

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderSettings.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sprim.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "host_copy.h"
#include "ies_tilt.h"
#include "instancer.h"
#include "light.h"
#include "material.h"
#include "mesh.h"
#include "motion_samples.h"
#include "sync_counter.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/ext/usd/render_settings.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd::internal {

namespace pxr = PXR_NS;

// The components of a Gf vector of 2 or 4 (GfVec2i, GfVec4d, ...), as T,
// read from its named fields.
template <typename T, std::size_t N, typename Vector>
std::array<T, N> Components(const Vector& vector) {
  static_assert(Vector::dimension == N);
  if constexpr (N == 2) {
    return {
        static_cast<T>(vector.x),
        static_cast<T>(vector.y),
    };
  } else {
    static_assert(N == 4);
    return {
        static_cast<T>(vector.x),
        static_cast<T>(vector.y),
        static_cast<T>(vector.z),
        static_cast<T>(vector.w),
    };
  }
}

// The attributes of a RenderSettings prim that its products inherit, as
// UsdRender resolves them (with the schema's fallbacks): the implicit
// product of settings without products takes them.
struct SettingsBase {
  std::array<std::int32_t, 2> resolution{2048, 1080};
  float pixel_aspect_ratio = 1.0F;
  pxr::TfToken aspect_ratio_conform_policy;
  std::array<float, 4> data_window_ndc{0.0F, 0.0F, 1.0F, 1.0F};
  MotionBlur motion_blur = MotionBlur::kEnabled;
  DepthOfField depth_of_field = DepthOfField::kEnabled;
};

// A RenderSettings prim, synced: Hydra's flattened products, and the
// attributes they inherit.
class RenderSettingsPrim final : public pxr::HdRenderSettings {
 public:
  // Counts its sync into `counter`, unless it is a fallback prim.
  RenderSettingsPrim(const pxr::SdfPath& prim_id,
                     SyncCounter* UMBRA_NULLABLE counter);

  [[nodiscard]] const SettingsBase& base() const UMBRA_LIFETIMEBOUND {
    return base_;
  }
  // The camera it names, an empty path for none.
  [[nodiscard]] pxr::SdfPath CameraPath() const;

 protected:
  // NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): the
  // name of the method OpenUSD lets render settings override.
  void _Sync(pxr::HdSceneDelegate* scene_delegate,
             pxr::HdRenderParam* UMBRA_NULLABLE render_param,
             const pxr::HdDirtyBits* dirty_bits) override;
  // NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

 private:
  SyncCounter* UMBRA_NULLABLE counter_;
  SettingsBase base_;
};

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

  SyncCounter* UMBRA_NULLABLE counter_;
  double meters_per_unit_;
  Changed changed_ = Changed::kNo;
  // The samples of its transform over its own shutter, in scene units: its
  // transform at the frame alone when that shutter spans no interval.
  TimedSamples<pxr::GfMatrix4d> transform_samples_;
  MotionOutcome motion_outcome_ = MotionOutcome::kSampled;
  host::Camera camera_;
};

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

// The change flags of a mesh converted whole: every member of its host copy
// is given.
inline constexpr device::ChangeFlags kWholeMesh =
    device::ChangeFlags::kPlacement | device::ChangeFlags::kTopology |
    device::ChangeFlags::kPrimvars | device::ChangeFlags::kMaterials |
    device::ChangeFlags::kEmission;

// The dirty bit of a mesh whose MeshLightAPI changed, which Hydra has none
// of: the delegate maps a change of a mesh's light data source to it.
inline constexpr pxr::HdDirtyBits kDirtyEmission =
    pxr::HdChangeTracker::CustomBitsBegin;

// A Mesh prim, synced into its host copy when it is visible and valid,
// with its instances: those its instancer places, flattened, or one of its
// own when it has no instancer. Implicit surfaces reach it as meshes. Its
// points are the output of its ext computation primvar `points` when it has
// one, as a skinned mesh does. Its points and its transform are sampled over
// the shutter, and its displayColor converted into ACEScg. Its material is
// the one bound to it, and its GeomSubsets bound to one keep theirs, each
// triangle given the index of its material: 0 for its own, i + 1 for that of
// its i-th subset. A mesh light's emission is read as a light's, and every
// instance emits it.
//
// A sync that finds it placed, visible with an instance or more, converts
// only what Hydra marked dirty, as change flags: its transform or its
// instancer's give kPlacement; its points kPoints, alone, or kPrimvars too
// when its normals are computed from them; its primvars or its normals
// kPrimvars, and kMaterials too when it gains or loses a displayColor; its
// material kMaterials; its topology or its sidedness kTopology, with
// kPrimvars and kMaterials; and its MeshLightAPI kEmission. Its host copy then
// holds only the members those flags name. Any other sync converts all of it.
class MeshPrim final : public pxr::HdMesh {
 public:
  // Counts its sync into `counter`; its lengths are scaled by
  // `meters_per_unit`. `shutter` is chosen before it syncs.
  MeshPrim(const pxr::SdfPath& prim_id, SyncCounter& counter,
           double meters_per_unit,
           const host::Shutter& shutter UMBRA_LIFETIMEBOUND);

  [[nodiscard]] pxr::HdDirtyBits GetInitialDirtyBitsMask() const override;
  void Sync(pxr::HdSceneDelegate* delegate,
            pxr::HdRenderParam* UMBRA_NULLABLE render_param,
            pxr::HdDirtyBits* dirty_bits, const pxr::TfToken& repr) override;

  [[nodiscard]] MeshState state() const { return state_; }
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
  // What its host copy holds since it was last committed: kWholeMesh when a
  // sync converted all of it, the members of its updates otherwise, and
  // kNone when it did not sync since.
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
  // Converts all of it.
  void Convert(pxr::HdSceneDelegate& delegate);
  // Converts what `bits` mark dirty, once placed.
  void Update(pxr::HdSceneDelegate& delegate, pxr::HdDirtyBits bits);
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
  // it is then converted, or invalid.
  void ConvertGeometry(pxr::HdSceneDelegate& delegate);
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
  MeshState state_ = MeshState::kNotSynced;
  MotionOutcome points_outcome_ = MotionOutcome::kSampled;
  MotionOutcome placement_outcome_ = MotionOutcome::kSampled;
  NormalsSource normals_ = NormalsSource::kAuthored;
  DisplayColor display_color_ = DisplayColor::kAbsent;
  std::size_t point_count_ = 0;
  std::size_t instance_count_ = 0;
  device::ChangeFlags changes_ = device::ChangeFlags::kNone;
  std::string subdivision_scheme_;
  std::string invalid_reason_;
  host::Mesh mesh_;
  std::vector<MaterialSubset> material_subsets_;
  std::vector<host::Instance> instances_;
  std::vector<LightNote> emission_notes_;
};

// An rprim of a type umbra does not take (basisCurves, points, volume):
// synced only to be counted, so that it is reported.
class UnsupportedPrim final : public pxr::HdRprim {
 public:
  // Counts its sync into `counter`.
  UnsupportedPrim(pxr::TfToken type_id, const pxr::SdfPath& prim_id,
                  SyncCounter& counter);

  [[nodiscard]] pxr::HdDirtyBits GetInitialDirtyBitsMask() const override;
  void Sync(pxr::HdSceneDelegate* delegate,
            pxr::HdRenderParam* UMBRA_NULLABLE render_param,
            pxr::HdDirtyBits* dirty_bits, const pxr::TfToken& repr) override;
  [[nodiscard]] const pxr::TfTokenVector& GetBuiltinPrimvarNames()
      const override;

  [[nodiscard]] const pxr::TfToken& type() const UMBRA_LIFETIMEBOUND {
    return type_;
  }
  // Whether Hydra synced it: whether its purpose is rendered.
  [[nodiscard]] bool IsSynced() const { return synced_ == Synced::kSynced; }

 protected:
  // NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): the
  // names of the methods OpenUSD has rprims override.
  [[nodiscard]] pxr::HdDirtyBits _PropagateDirtyBits(
      pxr::HdDirtyBits bits) const override;
  void _InitRepr(const pxr::TfToken& repr,
                 pxr::HdDirtyBits* dirty_bits) override;
  // NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

 private:
  enum class Synced : std::uint8_t { kNotSynced, kSynced };

  pxr::TfToken type_;
  SyncCounter* counter_;
  Synced synced_ = Synced::kNotSynced;
};

// The prims the delegate synced, by path, as CommitResources sees them, and
// the shutter they were sampled over.
struct SyncedPrims {
  host::Shutter shutter;
  std::map<pxr::SdfPath, const RenderSettingsPrim*> settings;
  std::map<pxr::SdfPath, CameraPrim*> cameras;
  std::map<pxr::SdfPath, MaterialPrim*> materials;
  std::map<pxr::SdfPath, MeshPrim*> meshes;
  std::map<pxr::SdfPath, const InstancerPrim*> instancers;
  std::map<pxr::SdfPath, const UnsupportedPrim*> unsupported;
  std::map<pxr::SdfPath, LightPrim*> lights;
  // The PortalLights placed that a dome light placed targets: those
  // committed.
  std::set<pxr::SdfPath> guided_portals;
};

// What a commit gives the host: the render settings, the parameters of the
// frame, and a description of what it committed, for the debug line that
// counts the prims.
struct Committed {
  RenderSettings settings;
  device::FrameParameters frame;
  std::string description;
};

// What commits the host copies of the synced prims to the devices: what it
// gives the host, or the error that stops the render.
using Committer =
    std::function<std::expected<Committed, core::Error>(SyncedPrims& prims)>;

// umbra's render delegate. It syncs cameras, render settings, materials,
// meshes and their instancers, and lights, with their lengths scaled by
// `meters_per_unit`, the lights inclined from the down direction of a stage
// whose up axis is `up_axis`, and the meshes bound to their materials for the
// full purpose, and counts the rprims and lights of the types umbra does not
// take; its prims count their syncs into the counter. Its ext computations,
// which the meshes run for their points, are Hydra's own, neither counted
// nor committed. Hydra creates and destroys its prims on one thread. The
// prims live for the whole render: each HdEngine::Execute syncs those that
// changed since the last, and commits them.
class RenderDelegate final : public pxr::HdRenderDelegate {
 public:
  RenderDelegate(SyncCounter& counter UMBRA_LIFETIMEBOUND,
                 double meters_per_unit, UpAxis up_axis, Committer committer);
  RenderDelegate(const RenderDelegate&) = delete;
  RenderDelegate& operator=(const RenderDelegate&) = delete;
  RenderDelegate(RenderDelegate&&) = delete;
  RenderDelegate& operator=(RenderDelegate&&) = delete;
  ~RenderDelegate() override;

  // The prims Hydra created, which it syncs: the total of the phase.
  [[nodiscard]] std::size_t PrimCount() const;

  // The render tags of the rprims to sync: the purposes the settings
  // include, once synced.
  [[nodiscard]] pxr::TfTokenVector RenderTags(
      const pxr::SdfPath& settings) const;

  // Chooses the shutter the rprims are sampled over, once the settings and
  // the cameras are synced: that of the settings' camera, or none when the
  // settings disable motion blur, the camera is not synced or its shutter
  // spans no interval. The lights that synced before it are placed over it,
  // and the rprims and the instancers that sync after it are sampled over it.
  void ChooseShutter(const pxr::SdfPath& settings);

  // Places the lights that synced since the last commit over the shutter
  // chosen, hiding those whose render tag is not in `render_tags`, then
  // finds the portals the domes guide. When a portal becomes guided, or no
  // longer is, the lights the commit needs are read again, if they did not
  // sync: the portal, when it becomes guided, and the placed domes that
  // target it.
  void Place(const pxr::TfTokenVector& render_tags);

  // What CommitResources committed, or its error; none before.
  [[nodiscard]] std::optional<std::expected<Committed, core::Error>>
  TakeCommitted();

  [[nodiscard]] const pxr::TfTokenVector& GetSupportedRprimTypes()
      const override;
  [[nodiscard]] const pxr::TfTokenVector& GetSupportedSprimTypes()
      const override;
  [[nodiscard]] const pxr::TfTokenVector& GetSupportedBprimTypes()
      const override;
  [[nodiscard]] pxr::TfToken GetMaterialBindingPurpose() const override;
  [[nodiscard]] pxr::TfTokenVector GetMaterialRenderContexts() const override;
  [[nodiscard]] pxr::TfTokenVector GetShaderSourceTypes() const override;
  [[nodiscard]] pxr::HdResourceRegistrySharedPtr GetResourceRegistry()
      const override;
  pxr::HdRenderPassSharedPtr CreateRenderPass(
      pxr::HdRenderIndex* index,
      const pxr::HdRprimCollection& collection) override;
  pxr::HdInstancer* CreateInstancer(pxr::HdSceneDelegate* delegate,
                                    const pxr::SdfPath& prim_id) override;
  void DestroyInstancer(pxr::HdInstancer* instancer) override;
  pxr::HdRprim* UMBRA_NULLABLE CreateRprim(
      const pxr::TfToken& type_id, const pxr::SdfPath& prim_id) override;
  void DestroyRprim(pxr::HdRprim* rprim) override;
  pxr::HdSprim* UMBRA_NULLABLE CreateSprim(
      const pxr::TfToken& type_id, const pxr::SdfPath& prim_id) override;
  pxr::HdSprim* UMBRA_NULLABLE
  CreateFallbackSprim(const pxr::TfToken& type_id) override;
  void DestroySprim(pxr::HdSprim* sprim) override;
  pxr::HdBprim* UMBRA_NULLABLE CreateBprim(
      const pxr::TfToken& type_id, const pxr::SdfPath& prim_id) override;
  pxr::HdBprim* UMBRA_NULLABLE
  CreateFallbackBprim(const pxr::TfToken& type_id) override;
  void DestroyBprim(pxr::HdBprim* bprim) override;
  void CommitResources(pxr::HdChangeTracker* tracker) override;

 private:
  // The shutter of `settings`: see ChooseShutter.
  [[nodiscard]] host::Shutter ShutterOf(const pxr::SdfPath& settings) const;
  // Counts the rprims Hydra did not sync into the counter's phase, which then
  // ends complete.
  void CountNotSynced();
  // Frees the host copies of every prim.
  void FreeHostCopies();
  // Finds the portals the domes guide: see Place.
  void GuidePortals(const pxr::TfTokenVector& render_tags);
  // Reads again the placed lights that target `portal`, if they did not
  // sync.
  void ReadDomesAgain(const pxr::SdfPath& portal,
                      const pxr::TfTokenVector& render_tags);

  SyncCounter* counter_;
  double meters_per_unit_;
  UpAxis up_axis_;
  Committer committer_;
  pxr::HdResourceRegistrySharedPtr registry_;
  SyncedPrims prims_;
  std::size_t commits_ = 0;
  std::optional<std::expected<Committed, core::Error>> committed_;
};

// The task of umbra's that runs in HdEngine::Execute: it only syncs, the
// rprims of the purposes the settings include, over the shutter it chooses.
class SyncTask final : public pxr::HdTask {
 public:
  // Syncs the prims of the render index whose render delegate is
  // `delegate`.
  SyncTask(RenderDelegate& delegate UMBRA_LIFETIMEBOUND, pxr::SdfPath settings);

  void Sync(pxr::HdSceneDelegate* UMBRA_NULLABLE delegate,
            pxr::HdTaskContext* context, pxr::HdDirtyBits* dirty_bits) override;
  void Prepare(pxr::HdTaskContext* context, pxr::HdRenderIndex* index) override;
  void Execute(pxr::HdTaskContext* context) override;
  [[nodiscard]] const pxr::TfTokenVector& GetRenderTags() const override;

 private:
  RenderDelegate* delegate_;
  pxr::SdfPath settings_;
  pxr::TfTokenVector render_tags_;
};

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_HYDRA_H_
