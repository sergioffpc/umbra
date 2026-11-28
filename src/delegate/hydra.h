// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// umbra's side of Hydra (docs/adr/0012-umbra-is-a-hydra-renderer.md): the
// render delegate, created directly rather than as a plugin, which creates
// the prims, each in a file of its own, that convert what they sync into
// their host copies. Hydra syncs rprims in parallel, each into its own prim.
// The delegate's CommitResources commits the host copies to the devices,
// then frees them (docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_DELEGATE_HYDRA_H_
#define UMBRA_DELEGATE_HYDRA_H_

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sprim.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>

#include "camera_prim.h"  // IWYU pragma: export
#include "dicing.h"
#include "host_copy.h"
#include "ies_tilt.h"
#include "instancer.h"
#include "light.h"
#include "material.h"
#include "mesh_prim.h"             // IWYU pragma: export
#include "render_settings_prim.h"  // IWYU pragma: export
#include "sync_counter.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/device/device.h"
#include "unsupported_prim.h"  // IWYU pragma: export

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

namespace pxr = PXR_NS;

// The prims the delegate synced, by path, as CommitResources sees them, and
// the shutter they were sampled over. Their headers come with this one,
// which the commit reads them through.
struct SyncedPrims {
  host::Shutter shutter;
  DicingView dicing;
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
// counts the prims, built only when the debug log is on, and empty
// otherwise.
struct Committed {
  RenderSettings settings;
  device::FrameParameters frame;
  std::string description;
};

// What commits the host copies of the synced prims to the devices: what it
// gives the host, or the error that stops the render.
using Committer =
    std::function<std::expected<Committed, core::Error>(SyncedPrims& prims)>;

// umbra's render delegate, which renders on `devices`. It syncs cameras, render
// settings, materials, meshes and their instancers, and lights, with their
// lengths scaled by `meters_per_unit`, the lights inclined from the down
// direction of a stage whose up axis is `up_axis`, and the meshes bound to
// their materials for the full purpose, and counts the rprims and lights of the
// types umbra does not take; its prims count their syncs into the counter. Its
// ext computations, which the meshes run for their points, are Hydra's own,
// neither counted nor committed. Hydra creates and destroys its prims on one
// thread. The prims live for the whole render: each HdEngine::Execute syncs
// those that changed since the last, and commits them.
class RenderDelegate final : public pxr::HdRenderDelegate {
 public:
  RenderDelegate(SyncCounter& counter UMBRA_LIFETIMEBOUND,
                 double meters_per_unit, UpAxis up_axis, Committer committer,
                 std::span<device::Device* const> devices UMBRA_LIFETIMEBOUND);
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

  // Chooses the view the subdivided meshes are diced for, once the shutter
  // is chosen: the settings' camera over the shutter, its image, the
  // largest of those of the products that see through it, and the settings'
  // dicing rate. When the view changes, every refined mesh is refined again
  // (MeshPrim::RefineForView), in parallel. Hydra cannot be asked to sync
  // them: it takes no invalidation while it syncs.
  void ChooseDicing(const pxr::SdfPath& settings);

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
  // What CommitResources committed, if it did and did not fail, which
  // TakeCommitted has not taken.
  [[nodiscard]] const Committed* UMBRA_NULLABLE
  PeekCommitted() const UMBRA_LIFETIMEBOUND;

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
  // umbra's RenderPass, on the delegate's devices.
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
  // The dicing view of `settings`: see ChooseDicing.
  [[nodiscard]] DicingView DicingOf(const pxr::SdfPath& settings) const;
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
  std::span<device::Device* const> devices_;
  pxr::HdResourceRegistrySharedPtr registry_;
  SyncedPrims prims_;
  std::size_t commits_ = 0;
  std::optional<std::expected<Committed, core::Error>> committed_;
};

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_HYDRA_H_
