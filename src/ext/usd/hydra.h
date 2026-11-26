// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// umbra's side of Hydra (docs/adr/0006-scenes-enter-through-hydra.md): the
// render delegate, created directly rather than as a plugin, its prims, which
// sync what the Scene needs, and the task that runs in HdEngine::Execute and
// only syncs. Hydra syncs rprims in parallel, each into its own prim. The
// delegate's CommitResources closes the Scene.

#ifndef UMBRA_EXT_USD_HYDRA_H_
#define UMBRA_EXT_USD_HYDRA_H_

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderSettings.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sprim.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "instancer.h"
#include "umbra/core/error.h"
#include "umbra/core/progress.h"
#include "umbra/core/safety.h"
#include "umbra/scene/scene.h"

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
  scene::MotionBlur motion_blur = scene::MotionBlur::kEnabled;
  scene::DepthOfField depth_of_field = scene::DepthOfField::kEnabled;
};

// A RenderSettings prim, synced: Hydra's flattened products, and the
// attributes they inherit.
class RenderSettingsPrim final : public pxr::HdRenderSettings {
 public:
  // Counts its sync into `phase`, unless it is a fallback prim.
  RenderSettingsPrim(const pxr::SdfPath& prim_id,
                     core::ProgressPhase* UMBRA_NULLABLE phase);

  [[nodiscard]] const SettingsBase& base() const UMBRA_LIFETIMEBOUND {
    return base_;
  }

 protected:
  // NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): the
  // name of the method OpenUSD lets render settings override.
  void _Sync(pxr::HdSceneDelegate* scene_delegate,
             pxr::HdRenderParam* UMBRA_NULLABLE render_param,
             const pxr::HdDirtyBits* dirty_bits) override;
  // NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

 private:
  core::ProgressPhase* UMBRA_NULLABLE phase_;
  SettingsBase base_;
};

// A Camera prim, synced.
class CameraPrim final : public pxr::HdCamera {
 public:
  // Counts its sync into `phase`, unless it is a fallback prim.
  CameraPrim(const pxr::SdfPath& prim_id,
             core::ProgressPhase* UMBRA_NULLABLE phase);

  void Sync(pxr::HdSceneDelegate* scene_delegate,
            pxr::HdRenderParam* UMBRA_NULLABLE render_param,
            pxr::HdDirtyBits* dirty_bits) override;

 private:
  core::ProgressPhase* UMBRA_NULLABLE phase_;
};

// What the sync of a mesh gave.
enum class MeshState : std::uint8_t {
  // It was not synced: its purpose is not rendered.
  kNotSynced,
  // It is invisible.
  kInvisible,
  // It is converted into the Scene's, with its instances.
  kConverted,
  // It is invalid, and dropped.
  kInvalid,
};

// A Mesh prim, synced into the Scene's mesh when it is visible and valid,
// with its instances: those its instancer places, flattened, or one of its
// own when it has no instancer. Implicit surfaces reach it as meshes.
class MeshPrim final : public pxr::HdMesh {
 public:
  // Counts its sync into `phase`; its lengths are scaled by
  // `meters_per_unit`.
  MeshPrim(const pxr::SdfPath& prim_id, core::ProgressPhase& phase,
           double meters_per_unit);

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
  // Its mesh, once converted, moved out.
  [[nodiscard]] scene::Mesh TakeMesh();
  // Its instances, once converted, moved out: their object-to-world
  // transforms in meters, and their own primvars; their prototype is not
  // set. None when its instancer places none.
  [[nodiscard]] std::vector<scene::Instance> TakeInstances();

 protected:
  // NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): the
  // names of the methods OpenUSD has rprims override.
  [[nodiscard]] pxr::HdDirtyBits _PropagateDirtyBits(
      pxr::HdDirtyBits bits) const override;
  void _InitRepr(const pxr::TfToken& repr,
                 pxr::HdDirtyBits* dirty_bits) override;
  // NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

 private:
  void Convert(pxr::HdSceneDelegate& delegate);
  // Its instances, from its transform and its instancer's.
  void Place(pxr::HdSceneDelegate& delegate);

  core::ProgressPhase* phase_;
  double meters_per_unit_;
  MeshState state_ = MeshState::kNotSynced;
  std::string subdivision_scheme_;
  std::string invalid_reason_;
  scene::Mesh mesh_;
  std::vector<scene::Instance> instances_;
};

// An rprim of a type the Scene does not take (basisCurves, points,
// volume): synced only to be counted, so that it is reported.
class UnsupportedPrim final : public pxr::HdRprim {
 public:
  // Counts its sync into `phase`.
  UnsupportedPrim(pxr::TfToken type_id, const pxr::SdfPath& prim_id,
                  core::ProgressPhase& phase);

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
  core::ProgressPhase* phase_;
  Synced synced_ = Synced::kNotSynced;
};

// The prims the delegate synced, by path, as CommitResources sees them.
struct SyncedPrims {
  std::map<pxr::SdfPath, const RenderSettingsPrim*> settings;
  std::map<pxr::SdfPath, const CameraPrim*> cameras;
  std::map<pxr::SdfPath, MeshPrim*> meshes;
  std::map<pxr::SdfPath, const InstancerPrim*> instancers;
  std::map<pxr::SdfPath, const UnsupportedPrim*> unsupported;
};

// What closes the Scene from the synced prims: the Scene, or the error that
// stops the render.
using SceneCloser = std::function<std::expected<scene::Scene, core::Error>(
    const SyncedPrims& prims)>;

// umbra's render delegate. It syncs cameras, render settings, meshes and
// their instancers, with their lengths scaled by `meters_per_unit`, and
// counts the rprims of the types the Scene does not take; its prims count
// their syncs into the phase. Hydra creates and destroys its prims on one
// thread.
class RenderDelegate final : public pxr::HdRenderDelegate {
 public:
  RenderDelegate(core::ProgressPhase& phase UMBRA_LIFETIMEBOUND,
                 double meters_per_unit, SceneCloser closer);
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

  // The Scene CommitResources closed, or its error; none before.
  [[nodiscard]] std::optional<std::expected<scene::Scene, core::Error>>
  TakeScene();

  [[nodiscard]] const pxr::TfTokenVector& GetSupportedRprimTypes()
      const override;
  [[nodiscard]] const pxr::TfTokenVector& GetSupportedSprimTypes()
      const override;
  [[nodiscard]] const pxr::TfTokenVector& GetSupportedBprimTypes()
      const override;
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
  // Counts the rprims Hydra did not sync into the phase, which then ends
  // complete.
  void CountNotSynced();

  core::ProgressPhase* phase_;
  double meters_per_unit_;
  SceneCloser closer_;
  pxr::HdResourceRegistrySharedPtr registry_;
  SyncedPrims prims_;
  std::optional<std::expected<scene::Scene, core::Error>> scene_;
};

// The task of umbra's that runs in HdEngine::Execute: it only syncs, the
// rprims of the purposes the settings include.
class SyncTask final : public pxr::HdTask {
 public:
  SyncTask(const RenderDelegate& delegate UMBRA_LIFETIMEBOUND,
           pxr::SdfPath settings);

  void Sync(pxr::HdSceneDelegate* UMBRA_NULLABLE delegate,
            pxr::HdTaskContext* context, pxr::HdDirtyBits* dirty_bits) override;
  void Prepare(pxr::HdTaskContext* context, pxr::HdRenderIndex* index) override;
  void Execute(pxr::HdTaskContext* context) override;
  [[nodiscard]] const pxr::TfTokenVector& GetRenderTags() const override;

 private:
  const RenderDelegate* delegate_;
  pxr::SdfPath settings_;
  pxr::TfTokenVector render_tags_;
};

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_HYDRA_H_
