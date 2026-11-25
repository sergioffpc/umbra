// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// umbra's side of Hydra (docs/adr/0006-scenes-enter-through-hydra.md): the
// render delegate, created directly rather than as a plugin, its prims, which
// sync what the Scene needs, and the task that runs in HdEngine::Execute and
// only syncs. The delegate's CommitResources closes the Scene.

#ifndef UMBRA_EXT_USD_HYDRA_H_
#define UMBRA_EXT_USD_HYDRA_H_

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/instancer.h>
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

// The prims the delegate synced, by path, as CommitResources sees them.
struct SyncedPrims {
  std::map<pxr::SdfPath, const RenderSettingsPrim*> settings;
  std::map<pxr::SdfPath, const CameraPrim*> cameras;
};

// What closes the Scene from the synced prims: the Scene, or the error that
// stops the render.
using SceneCloser = std::function<std::expected<scene::Scene, core::Error>(
    const SyncedPrims& prims)>;

// umbra's render delegate. It syncs cameras and render settings; its prims
// count their syncs into the phase. Hydra creates and destroys its prims on
// one thread.
class RenderDelegate final : public pxr::HdRenderDelegate {
 public:
  RenderDelegate(core::ProgressPhase& phase UMBRA_LIFETIMEBOUND,
                 SceneCloser closer);
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
  core::ProgressPhase* phase_;
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
