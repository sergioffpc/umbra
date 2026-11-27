// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "hydra.h"

#include <pxr/base/tf/token.h>
#include <pxr/base/work/loops.h>
#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/extComputation.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sprim.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/renderSettingsFilteringSceneIndex.h>
#include <pxr/usd/sdf/path.h>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include "camera_prim.h"
#include "dicing.h"
#include "host_copy.h"
#include "ies_tilt.h"
#include "instancer.h"
#include "light.h"
#include "material.h"
#include "mesh_prim.h"
#include "motion_samples.h"
#include "render_pass.h"
#include "render_settings_prim.h"
#include "sync_counter.h"
#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/device/device.h"
#include "unsupported_prim.h"

namespace umbra::delegate::internal {
namespace {

// A prim for Hydra to own: it gives it back to its Destroy method.
template <typename Base, typename Prim>
Base* Release(std::unique_ptr<Prim> prim) {
  return prim.release();
}

// The purpose of prims with no other: "default".
const pxr::TfToken& DefaultPurpose() {
  static const pxr::TfToken kDefault("default");
  return kDefault;
}

// Forgets `prim`, about to be destroyed, when `prims` holds it.
template <typename Prim, typename Base>
void Forget(std::map<pxr::SdfPath, Prim*>& prims, const Base& prim) {
  const auto found = prims.find(prim.GetId());
  if (found != prims.end() && found->second == &prim) {
    prims.erase(found);
  }
}

// The rprim types umbra does not take, which are only counted.
const pxr::TfTokenVector& UnsupportedRprimTypes() {
  static const pxr::TfTokenVector kTypes = {
      pxr::HdPrimTypeTokens->basisCurves,
      pxr::HdPrimTypeTokens->points,
      pxr::HdPrimTypeTokens->volume,
  };
  return kTypes;
}

// The PortalLights of `lights` that are placed and that a placed dome light
// targets.
std::set<pxr::SdfPath> GuidedPortals(
    const std::map<pxr::SdfPath, LightPrim*>& lights) {
  const auto is_placed_portal = [&lights](const pxr::SdfPath& target) {
    const auto portal = lights.find(target);
    return portal != lights.end() && portal->second->IsPortal() &&
           portal->second->IsPlaced();
  };
  std::set<pxr::SdfPath> guided;
  for (const auto& [path, light] : lights) {
    if (light->IsPlaced()) {
      std::ranges::copy_if(light->portals(),
                           std::inserter(guided, guided.end()),
                           is_placed_portal);
    }
  }
  return guided;
}

// Whether the delegate syncs sprims of `type` as lights.
bool IsLightPrimType(const pxr::TfToken& type) {
  return std::ranges::find(LightPrimTypes(), type) != LightPrimTypes().end();
}

}  // namespace

RenderDelegate::RenderDelegate(SyncCounter& counter, double meters_per_unit,
                               UpAxis up_axis, Committer committer,
                               std::span<device::Device* const> devices)
    : counter_(&counter),
      meters_per_unit_(meters_per_unit),
      up_axis_(up_axis),
      committer_(std::move(committer)),
      devices_(devices),
      registry_(std::make_shared<pxr::HdResourceRegistry>()) {
  MapEmissionToDirtyBit();
}

RenderDelegate::~RenderDelegate() = default;

std::size_t RenderDelegate::PrimCount() const {
  return prims_.settings.size() + prims_.cameras.size() +
         prims_.materials.size() + prims_.meshes.size() +
         prims_.instancers.size() + prims_.unsupported.size() +
         prims_.lights.size();
}

host::Shutter RenderDelegate::ShutterOf(const pxr::SdfPath& settings) const {
  const auto found = prims_.settings.find(settings);
  if (found == prims_.settings.end() ||
      found->second->base().motion_blur == MotionBlur::kDisabled) {
    return {};
  }
  const auto camera = prims_.cameras.find(found->second->CameraPath());
  if (camera == prims_.cameras.end()) {
    return {};
  }
  const host::Shutter shutter{
      .open = camera->second->GetShutterOpen(),
      .close = camera->second->GetShutterClose(),
  };
  return IsMotionBlurred(shutter) ? shutter : host::Shutter{};
}

void RenderDelegate::ChooseShutter(const pxr::SdfPath& settings) {
  prims_.shutter = ShutterOf(settings);
}

DicingView RenderDelegate::DicingOf(const pxr::SdfPath& settings) const {
  const auto found = prims_.settings.find(settings);
  if (found == prims_.settings.end()) {
    return {};
  }
  const RenderSettingsPrim& prim = *found->second;
  const double rate = prim.dicing_rate().pixels;
  const auto camera = prims_.cameras.find(prim.CameraPath());
  if (camera == prims_.cameras.end()) {
    return {.scale = {}, .eyes = {}, .rate = rate};
  }
  return DicingViewOf(camera->second->Sampled(prims_.shutter), ImagesOf(prim),
                      rate);
}

void RenderDelegate::ChooseDicing(const pxr::SdfPath& settings) {
  DicingView view = DicingOf(settings);
  if (view == prims_.dicing) {
    return;
  }
  prims_.dicing = std::move(view);
  std::vector<MeshPrim*> refined;
  for (const auto& [path, mesh] : prims_.meshes) {
    if (mesh->IsRefined()) {
      refined.push_back(mesh);
    }
  }
  if (refined.empty()) {
    return;
  }
  pxr::WorkParallelForEach(refined.begin(), refined.end(),
                           [](MeshPrim* mesh) { mesh->RefineForView(); });
  UMBRA_LOG_DEBUG(kUsd, "the view changed: {} subdivided mesh{} refined again",
                  core::FormatCount(refined.size()),
                  refined.size() == 1 ? "" : "es");
}

void RenderDelegate::Place(const pxr::TfTokenVector& render_tags) {
  for (const auto& [path, light] : prims_.lights) {
    if (light->IsChanged()) {
      light->Place(prims_.shutter, render_tags);
    }
  }
  GuidePortals(render_tags);
}

void RenderDelegate::GuidePortals(const pxr::TfTokenVector& render_tags) {
  std::set<pxr::SdfPath> guided = GuidedPortals(prims_.lights);
  std::vector<pxr::SdfPath> flipped;
  std::ranges::set_symmetric_difference(guided, prims_.guided_portals,
                                        std::back_inserter(flipped));
  prims_.guided_portals = std::move(guided);
  for (const pxr::SdfPath& portal : flipped) {
    // A portal committed anew needs its host copy; a dome, the ids of its
    // portals.
    const auto found = prims_.lights.find(portal);
    if (found != prims_.lights.end() &&
        prims_.guided_portals.contains(portal)) {
      found->second->ReadAgain(prims_.shutter, render_tags);
    }
    ReadDomesAgain(portal, render_tags);
  }
}

void RenderDelegate::ReadDomesAgain(const pxr::SdfPath& portal,
                                    const pxr::TfTokenVector& render_tags) {
  for (const auto& [path, light] : prims_.lights) {
    if (light->IsPlaced() && std::ranges::contains(light->portals(), portal)) {
      light->ReadAgain(prims_.shutter, render_tags);
    }
  }
}

pxr::TfTokenVector RenderDelegate::RenderTags(
    const pxr::SdfPath& settings) const {
  const auto found = prims_.settings.find(settings);
  if (found == prims_.settings.end()) {
    return {};
  }
  pxr::TfTokenVector tags;
  for (const pxr::TfToken& purpose : found->second->GetIncludedPurposes()) {
    // Hydra tags the rprims of the default purpose "geometry".
    tags.push_back(purpose == DefaultPurpose()
                       ? pxr::HdRenderTagTokens->geometry
                       : purpose);
  }
  return tags;
}

std::optional<std::expected<Committed, core::Error>>
RenderDelegate::TakeCommitted() {
  return std::exchange(committed_, std::nullopt);
}

const Committed* UMBRA_NULLABLE RenderDelegate::PeekCommitted() const {
  return committed_.has_value() && committed_->has_value() ? &**committed_
                                                           : nullptr;
}

const pxr::TfTokenVector& RenderDelegate::GetSupportedRprimTypes() const {
  static const pxr::TfTokenVector kTypes = [] {
    pxr::TfTokenVector types = {pxr::HdPrimTypeTokens->mesh};
    const pxr::TfTokenVector& unsupported = UnsupportedRprimTypes();
    types.insert(types.end(), unsupported.begin(), unsupported.end());
    return types;
  }();
  return kTypes;
}

const pxr::TfTokenVector& RenderDelegate::GetSupportedSprimTypes() const {
  static const pxr::TfTokenVector kTypes = [] {
    pxr::TfTokenVector types = {
        pxr::HdPrimTypeTokens->camera,
        pxr::HdPrimTypeTokens->material,
        pxr::HdPrimTypeTokens->extComputation,
    };
    const pxr::TfTokenVector& lights = LightPrimTypes();
    types.insert(types.end(), lights.begin(), lights.end());
    return types;
  }();
  return kTypes;
}

const pxr::TfTokenVector& RenderDelegate::GetSupportedBprimTypes() const {
  static const pxr::TfTokenVector kTypes = {
      pxr::HdPrimTypeTokens->renderSettings,
  };
  return kTypes;
}

pxr::TfToken RenderDelegate::GetMaterialBindingPurpose() const {
  // umbra renders the final image: the full purpose, then allPurpose.
  return pxr::HdTokens->full;
}

pxr::TfTokenVector RenderDelegate::GetMaterialRenderContexts() const {
  return MaterialRenderContexts();
}

pxr::TfTokenVector RenderDelegate::GetShaderSourceTypes() const {
  return ShaderSourceTypes();
}

pxr::HdResourceRegistrySharedPtr RenderDelegate::GetResourceRegistry() const {
  return registry_;
}

pxr::HdRenderPassSharedPtr RenderDelegate::CreateRenderPass(
    pxr::HdRenderIndex* index, const pxr::HdRprimCollection& collection) {
  return std::make_shared<RenderPass>(index, collection, devices_);
}

pxr::HdInstancer* RenderDelegate::CreateInstancer(
    pxr::HdSceneDelegate* delegate, const pxr::SdfPath& prim_id) {
  auto instancer = std::make_unique<InstancerPrim>(delegate, prim_id, *counter_,
                                                   prims_.shutter);
  prims_.instancers[prim_id] = instancer.get();
  return Release<pxr::HdInstancer>(std::move(instancer));
}

void RenderDelegate::DestroyInstancer(pxr::HdInstancer* instancer) {
  const std::unique_ptr<pxr::HdInstancer> owned(instancer);
  Forget(prims_.instancers, *instancer);
}

pxr::HdRprim* UMBRA_NULLABLE RenderDelegate::CreateRprim(
    const pxr::TfToken& type_id, const pxr::SdfPath& prim_id) {
  if (type_id == pxr::HdPrimTypeTokens->mesh) {
    auto mesh = std::make_unique<MeshPrim>(prim_id, *counter_, meters_per_unit_,
                                           prims_.shutter, prims_.dicing);
    prims_.meshes[prim_id] = mesh.get();
    return Release<pxr::HdRprim>(std::move(mesh));
  }
  for (const pxr::TfToken& unsupported : UnsupportedRprimTypes()) {
    if (type_id == unsupported) {
      auto prim =
          std::make_unique<UnsupportedPrim>(type_id, prim_id, *counter_);
      prims_.unsupported[prim_id] = prim.get();
      return Release<pxr::HdRprim>(std::move(prim));
    }
  }
  return nullptr;
}

void RenderDelegate::DestroyRprim(pxr::HdRprim* rprim) {
  const std::unique_ptr<pxr::HdRprim> owned(rprim);
  Forget(prims_.meshes, *rprim);
  Forget(prims_.unsupported, *rprim);
}

pxr::HdSprim* UMBRA_NULLABLE RenderDelegate::CreateSprim(
    const pxr::TfToken& type_id, const pxr::SdfPath& prim_id) {
  if (IsLightPrimType(type_id)) {
    auto light = std::make_unique<LightPrim>(type_id, prim_id, counter_,
                                             meters_per_unit_, up_axis_);
    prims_.lights[prim_id] = light.get();
    return Release<pxr::HdSprim>(std::move(light));
  }
  if (type_id == pxr::HdPrimTypeTokens->material) {
    auto material = std::make_unique<MaterialPrim>(prim_id, counter_);
    prims_.materials[prim_id] = material.get();
    return Release<pxr::HdSprim>(std::move(material));
  }
  // An ext computation holds no render data: the meshes whose primvars it
  // computes run it as they sync. It is neither counted nor committed.
  if (type_id == pxr::HdPrimTypeTokens->extComputation) {
    return Release<pxr::HdSprim>(
        std::make_unique<pxr::HdExtComputation>(prim_id));
  }
  if (type_id != pxr::HdPrimTypeTokens->camera) {
    return nullptr;
  }
  auto camera =
      std::make_unique<CameraPrim>(prim_id, counter_, meters_per_unit_);
  prims_.cameras[prim_id] = camera.get();
  return Release<pxr::HdSprim>(std::move(camera));
}

pxr::HdSprim* UMBRA_NULLABLE
RenderDelegate::CreateFallbackSprim(const pxr::TfToken& type_id) {
  if (IsLightPrimType(type_id)) {
    return Release<pxr::HdSprim>(
        std::make_unique<LightPrim>(type_id, pxr::SdfPath::EmptyPath(), nullptr,
                                    meters_per_unit_, up_axis_));
  }
  if (type_id == pxr::HdPrimTypeTokens->material) {
    return Release<pxr::HdSprim>(
        std::make_unique<MaterialPrim>(pxr::SdfPath::EmptyPath(), nullptr));
  }
  if (type_id == pxr::HdPrimTypeTokens->extComputation) {
    return Release<pxr::HdSprim>(
        std::make_unique<pxr::HdExtComputation>(pxr::SdfPath::EmptyPath()));
  }
  if (type_id != pxr::HdPrimTypeTokens->camera) {
    return nullptr;
  }
  return Release<pxr::HdSprim>(std::make_unique<CameraPrim>(
      pxr::SdfPath::EmptyPath(), nullptr, meters_per_unit_));
}

void RenderDelegate::DestroySprim(pxr::HdSprim* sprim) {
  const std::unique_ptr<pxr::HdSprim> owned(sprim);
  Forget(prims_.cameras, *sprim);
  Forget(prims_.materials, *sprim);
  Forget(prims_.lights, *sprim);
}

pxr::HdBprim* UMBRA_NULLABLE RenderDelegate::CreateBprim(
    const pxr::TfToken& type_id, const pxr::SdfPath& prim_id) {
  if (type_id != pxr::HdPrimTypeTokens->renderSettings) {
    return nullptr;
  }
  // hdsi's render settings filter adds a fallback prim of its own, which is
  // not the stage's: it is not counted, and never the settings rendered.
  if (prim_id ==
      pxr::HdsiRenderSettingsFilteringSceneIndex::GetFallbackPrimPath()) {
    return Release<pxr::HdBprim>(
        std::make_unique<RenderSettingsPrim>(prim_id, nullptr));
  }
  auto settings = std::make_unique<RenderSettingsPrim>(prim_id, counter_);
  prims_.settings[prim_id] = settings.get();
  return Release<pxr::HdBprim>(std::move(settings));
}

pxr::HdBprim* UMBRA_NULLABLE
RenderDelegate::CreateFallbackBprim(const pxr::TfToken& type_id) {
  if (type_id != pxr::HdPrimTypeTokens->renderSettings) {
    return nullptr;
  }
  return Release<pxr::HdBprim>(
      std::make_unique<RenderSettingsPrim>(pxr::SdfPath::EmptyPath(), nullptr));
}

void RenderDelegate::DestroyBprim(pxr::HdBprim* bprim) {
  const std::unique_ptr<pxr::HdBprim> owned(bprim);
  Forget(prims_.settings, *bprim);
}

void RenderDelegate::CountNotSynced() {
  for (const auto& [path, mesh] : prims_.meshes) {
    if (mesh->state() == MeshState::kNotSynced) {
      counter_->Add(1);
    }
  }
  for (const auto& [path, instancer] : prims_.instancers) {
    if (!instancer->IsSynced()) {
      counter_->Add(1);
    }
  }
  for (const auto& [path, prim] : prims_.unsupported) {
    if (!prim->IsSynced()) {
      counter_->Add(1);
    }
  }
}

void RenderDelegate::FreeHostCopies() {
  for (const auto& [path, camera] : prims_.cameras) {
    camera->FreeHostCopy();
  }
  for (const auto& [path, material] : prims_.materials) {
    material->FreeHostCopy();
  }
  for (const auto& [path, mesh] : prims_.meshes) {
    mesh->FreeHostCopy();
  }
  for (const auto& [path, light] : prims_.lights) {
    light->FreeHostCopy();
  }
}

void RenderDelegate::CommitResources(pxr::HdChangeTracker* /*tracker*/) {
  if (commits_ == 0) {
    CountNotSynced();
  }
  committed_ = committer_(prims_);
  ++commits_;
  // No host copy outlives the commit, whether it succeeds or not.
  FreeHostCopies();
}

}  // namespace umbra::delegate::internal
