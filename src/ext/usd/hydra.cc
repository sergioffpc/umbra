// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "hydra.h"

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/enums.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderSettings.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/sprim.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hdsi/renderSettingsFilteringSceneIndex.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usdImaging/usdImaging/usdRenderSettingsSchema.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "mesh.h"
#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/core/progress.h"
#include "umbra/core/safety.h"
#include "umbra/scene/scene.h"

namespace umbra::ext::usd::internal {
namespace {

// Counts the sync of the prim at `prim_id` into `phase`, for a prim that is not
// a fallback.
void CountSync(core::ProgressPhase* UMBRA_NULLABLE phase,
               const pxr::SdfPath& prim_id) {
  if (phase == nullptr) {
    return;
  }
  phase->Add(1);
  UMBRA_LOG_TRACE(kUsd, "synced {}", prim_id.GetString());
}

template <typename T>
using Handle = std::shared_ptr<pxr::HdTypedSampledDataSource<T>>;

// The value of `source`, or `fallback` when it has none.
template <typename T>
T ValueOr(const Handle<T>& source, T fallback) {
  return source != nullptr ? source->GetTypedValue(0.0F) : std::move(fallback);
}

// The settings' attributes that their products inherit, from the prim's
// UsdRender data source in the terminal scene index.
SettingsBase ReadSettingsBase(const pxr::HdContainerDataSourceHandle& prim) {
  const pxr::UsdImagingUsdRenderSettingsSchema schema =
      pxr::UsdImagingUsdRenderSettingsSchema::GetFromParent(prim);
  SettingsBase base;
  if (const auto resolution = schema.GetResolution(); resolution != nullptr) {
    base.resolution =
        Components<std::int32_t, 2>(resolution->GetTypedValue(0.0F));
  }
  base.pixel_aspect_ratio =
      ValueOr(schema.GetPixelAspectRatio(), base.pixel_aspect_ratio);
  base.aspect_ratio_conform_policy =
      ValueOr(schema.GetAspectRatioConformPolicy(), pxr::TfToken());
  if (const auto window = schema.GetDataWindowNDC(); window != nullptr) {
    base.data_window_ndc = Components<float, 4>(window->GetTypedValue(0.0F));
  }
  if (ValueOr(schema.GetDisableMotionBlur(), false)) {
    base.motion_blur = scene::MotionBlur::kDisabled;
  }
  if (ValueOr(schema.GetDisableDepthOfField(), false)) {
    base.depth_of_field = scene::DepthOfField::kDisabled;
  }
  return base;
}

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

// The primvars of the prim at `prim_id`, with their indices when indexed.
std::vector<SourcePrimvar> ReadPrimvars(pxr::HdSceneDelegate& delegate,
                                        const pxr::SdfPath& prim_id) {
  static constexpr std::array kInterpolations = {
      pxr::HdInterpolationConstant,    pxr::HdInterpolationUniform,
      pxr::HdInterpolationVarying,     pxr::HdInterpolationVertex,
      pxr::HdInterpolationFaceVarying,
  };
  std::vector<SourcePrimvar> primvars;
  for (const pxr::HdInterpolation interpolation : kInterpolations) {
    for (const pxr::HdPrimvarDescriptor& descriptor :
         delegate.GetPrimvarDescriptors(prim_id, interpolation)) {
      SourcePrimvar primvar{
          .name = descriptor.name,
          .interpolation = interpolation,
          .value = {},
          .indices = {},
      };
      primvar.value = descriptor.indexed
                          ? delegate.GetIndexedPrimvar(prim_id, descriptor.name,
                                                       &primvar.indices)
                          : delegate.Get(prim_id, descriptor.name);
      primvars.push_back(std::move(primvar));
    }
  }
  return primvars;
}

// Forgets `prim`, about to be destroyed, when `prims` holds it.
template <typename Prim, typename Base>
void Forget(std::map<pxr::SdfPath, Prim*>& prims, const Base& prim) {
  const auto found = prims.find(prim.GetId());
  if (found != prims.end() && found->second == &prim) {
    prims.erase(found);
  }
}

// The rprim types the Scene does not take, which are only counted.
const pxr::TfTokenVector& UnsupportedRprimTypes() {
  static const pxr::TfTokenVector kTypes = {
      pxr::HdPrimTypeTokens->basisCurves,
      pxr::HdPrimTypeTokens->points,
      pxr::HdPrimTypeTokens->volume,
  };
  return kTypes;
}

// The path of the task, which is not in the render index.
const pxr::SdfPath& SyncTaskPath() {
  static const pxr::SdfPath kPath("/__umbraSyncTask");
  return kPath;
}

}  // namespace

RenderSettingsPrim::RenderSettingsPrim(
    const pxr::SdfPath& prim_id, core::ProgressPhase* UMBRA_NULLABLE phase)
    : pxr::HdRenderSettings(prim_id), phase_(phase) {}

void RenderSettingsPrim::_Sync(
    pxr::HdSceneDelegate* scene_delegate,
    pxr::HdRenderParam* UMBRA_NULLABLE /*render_param*/,
    const pxr::HdDirtyBits* /*dirty_bits*/) {
  const pxr::HdSceneIndexBaseRefPtr terminal =
      scene_delegate->GetRenderIndex().GetTerminalSceneIndex();
  if (terminal != nullptr) {
    base_ = ReadSettingsBase(terminal->GetPrim(GetId()).dataSource);
  }
  CountSync(phase_, GetId());
}

CameraPrim::CameraPrim(const pxr::SdfPath& prim_id,
                       core::ProgressPhase* UMBRA_NULLABLE phase)
    : pxr::HdCamera(prim_id), phase_(phase) {}

void CameraPrim::Sync(pxr::HdSceneDelegate* scene_delegate,
                      pxr::HdRenderParam* UMBRA_NULLABLE render_param,
                      pxr::HdDirtyBits* dirty_bits) {
  pxr::HdCamera::Sync(scene_delegate, render_param, dirty_bits);
  CountSync(phase_, GetId());
}

MeshPrim::MeshPrim(const pxr::SdfPath& prim_id, core::ProgressPhase& phase,
                   double meters_per_unit)
    : pxr::HdMesh(prim_id), phase_(&phase), meters_per_unit_(meters_per_unit) {}

pxr::HdDirtyBits MeshPrim::GetInitialDirtyBitsMask() const {
  return pxr::HdChangeTracker::AllSceneDirtyBits;
}

void MeshPrim::Sync(pxr::HdSceneDelegate* delegate,
                    pxr::HdRenderParam* UMBRA_NULLABLE /*render_param*/,
                    pxr::HdDirtyBits* dirty_bits,
                    const pxr::TfToken& /*repr*/) {
  _UpdateVisibility(delegate, dirty_bits);
  if (!IsVisible()) {
    state_ = MeshState::kInvisible;
  } else if (!delegate->GetInstancerId(GetId()).IsEmpty()) {
    state_ = MeshState::kInstanced;
  } else {
    Convert(*delegate);
  }
  *dirty_bits &= ~pxr::HdChangeTracker::AllSceneDirtyBits;
  phase_->Add(1);
  if (state_ == MeshState::kConverted) {
    const std::size_t triangles = mesh_.triangles.size();
    UMBRA_LOG_TRACE(kUsd, "synced {} ({} triangle{})", GetId().GetString(),
                    core::FormatCount(triangles), triangles == 1 ? "" : "s");
  } else {
    UMBRA_LOG_TRACE(kUsd, "synced {}", GetId().GetString());
  }
}

void MeshPrim::Convert(pxr::HdSceneDelegate& delegate) {
  const SourceMesh source{
      .path = GetId().GetString(),
      .topology = GetMeshTopology(&delegate),
      .points = GetPoints(&delegate),
      .primvars = ReadPrimvars(delegate, GetId()),
      .sidedness = IsDoubleSided(&delegate) ? scene::Sidedness::kDoubleSided
                                            : scene::Sidedness::kSingleSided,
  };
  if (IsSubdivided(source.topology)) {
    subdivision_scheme_ = source.topology.GetScheme().GetString();
  }
  auto mesh = ConvertMesh(source, meters_per_unit_);
  if (!mesh.has_value()) {
    state_ = MeshState::kInvalid;
    invalid_reason_ = std::move(mesh.error());
    return;
  }
  state_ = MeshState::kConverted;
  mesh_ = std::move(*mesh);
  object_to_world_ =
      TransformInMeters(delegate.GetTransform(GetId()), meters_per_unit_);
}

scene::Mesh MeshPrim::TakeMesh() { return std::move(mesh_); }

pxr::HdDirtyBits MeshPrim::_PropagateDirtyBits(pxr::HdDirtyBits bits) const {
  return bits;
}

void MeshPrim::_InitRepr(const pxr::TfToken& /*repr*/,
                         pxr::HdDirtyBits* /*dirty_bits*/) {
  // umbra draws no repr: it only syncs.
}

UnsupportedPrim::UnsupportedPrim(pxr::TfToken type_id,
                                 const pxr::SdfPath& prim_id,
                                 core::ProgressPhase& phase)
    : pxr::HdRprim(prim_id), type_(std::move(type_id)), phase_(&phase) {}

pxr::HdDirtyBits UnsupportedPrim::GetInitialDirtyBitsMask() const {
  return pxr::HdChangeTracker::AllSceneDirtyBits;
}

void UnsupportedPrim::Sync(pxr::HdSceneDelegate* /*delegate*/,
                           pxr::HdRenderParam* UMBRA_NULLABLE /*render_param*/,
                           pxr::HdDirtyBits* dirty_bits,
                           const pxr::TfToken& /*repr*/) {
  *dirty_bits &= ~pxr::HdChangeTracker::AllSceneDirtyBits;
  synced_ = Synced::kSynced;
  CountSync(phase_, GetId());
}

const pxr::TfTokenVector& UnsupportedPrim::GetBuiltinPrimvarNames() const {
  static const pxr::TfTokenVector kNames;
  return kNames;
}

pxr::HdDirtyBits UnsupportedPrim::_PropagateDirtyBits(
    pxr::HdDirtyBits bits) const {
  return bits;
}

void UnsupportedPrim::_InitRepr(const pxr::TfToken& /*repr*/,
                                pxr::HdDirtyBits* /*dirty_bits*/) {
  // umbra draws no repr: it only counts.
}

RenderDelegate::RenderDelegate(core::ProgressPhase& phase,
                               double meters_per_unit, SceneCloser closer)
    : phase_(&phase),
      meters_per_unit_(meters_per_unit),
      closer_(std::move(closer)),
      registry_(std::make_shared<pxr::HdResourceRegistry>()) {}

RenderDelegate::~RenderDelegate() = default;

std::size_t RenderDelegate::PrimCount() const {
  return prims_.settings.size() + prims_.cameras.size() + prims_.meshes.size() +
         prims_.unsupported.size();
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

std::optional<std::expected<scene::Scene, core::Error>>
RenderDelegate::TakeScene() {
  return std::exchange(scene_, std::nullopt);
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
  static const pxr::TfTokenVector kTypes = {pxr::HdPrimTypeTokens->camera};
  return kTypes;
}

const pxr::TfTokenVector& RenderDelegate::GetSupportedBprimTypes() const {
  static const pxr::TfTokenVector kTypes = {
      pxr::HdPrimTypeTokens->renderSettings,
  };
  return kTypes;
}

pxr::HdResourceRegistrySharedPtr RenderDelegate::GetResourceRegistry() const {
  return registry_;
}

pxr::HdRenderPassSharedPtr RenderDelegate::CreateRenderPass(
    pxr::HdRenderIndex* /*index*/,
    const pxr::HdRprimCollection& /*collection*/) {
  // umbra renders outside Hydra: it has no render pass.
  return nullptr;
}

pxr::HdInstancer* RenderDelegate::CreateInstancer(
    pxr::HdSceneDelegate* delegate, const pxr::SdfPath& prim_id) {
  return Release<pxr::HdInstancer>(
      std::make_unique<pxr::HdInstancer>(delegate, prim_id));
}

void RenderDelegate::DestroyInstancer(pxr::HdInstancer* instancer) {
  const std::unique_ptr<pxr::HdInstancer> owned(instancer);
}

pxr::HdRprim* UMBRA_NULLABLE RenderDelegate::CreateRprim(
    const pxr::TfToken& type_id, const pxr::SdfPath& prim_id) {
  if (type_id == pxr::HdPrimTypeTokens->mesh) {
    auto mesh = std::make_unique<MeshPrim>(prim_id, *phase_, meters_per_unit_);
    prims_.meshes[prim_id] = mesh.get();
    return Release<pxr::HdRprim>(std::move(mesh));
  }
  for (const pxr::TfToken& unsupported : UnsupportedRprimTypes()) {
    if (type_id == unsupported) {
      auto prim = std::make_unique<UnsupportedPrim>(type_id, prim_id, *phase_);
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
  if (type_id != pxr::HdPrimTypeTokens->camera) {
    return nullptr;
  }
  auto camera = std::make_unique<CameraPrim>(prim_id, phase_);
  prims_.cameras[prim_id] = camera.get();
  return Release<pxr::HdSprim>(std::move(camera));
}

pxr::HdSprim* UMBRA_NULLABLE
RenderDelegate::CreateFallbackSprim(const pxr::TfToken& type_id) {
  if (type_id != pxr::HdPrimTypeTokens->camera) {
    return nullptr;
  }
  return Release<pxr::HdSprim>(
      std::make_unique<CameraPrim>(pxr::SdfPath::EmptyPath(), nullptr));
}

void RenderDelegate::DestroySprim(pxr::HdSprim* sprim) {
  const std::unique_ptr<pxr::HdSprim> owned(sprim);
  Forget(prims_.cameras, *sprim);
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
  auto settings = std::make_unique<RenderSettingsPrim>(prim_id, phase_);
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
      phase_->Add(1);
    }
  }
  for (const auto& [path, prim] : prims_.unsupported) {
    if (!prim->IsSynced()) {
      phase_->Add(1);
    }
  }
}

void RenderDelegate::CommitResources(pxr::HdChangeTracker* /*tracker*/) {
  // The Scene is closed once: it is immutable once loaded.
  if (!scene_.has_value()) {
    CountNotSynced();
    scene_ = closer_(prims_);
  }
}

SyncTask::SyncTask(const RenderDelegate& delegate, pxr::SdfPath settings)
    : pxr::HdTask(SyncTaskPath()),
      delegate_(&delegate),
      settings_(std::move(settings)) {}

void SyncTask::Sync(pxr::HdSceneDelegate* UMBRA_NULLABLE /*delegate*/,
                    pxr::HdTaskContext* /*context*/,
                    pxr::HdDirtyBits* dirty_bits) {
  // The render settings are synced before the tasks, and the rprims after.
  render_tags_ = delegate_->RenderTags(settings_);
  *dirty_bits = pxr::HdChangeTracker::Clean;
}

void SyncTask::Prepare(pxr::HdTaskContext* /*context*/,
                       pxr::HdRenderIndex* /*index*/) {}

void SyncTask::Execute(pxr::HdTaskContext* /*context*/) {}

const pxr::TfTokenVector& SyncTask::GetRenderTags() const {
  return render_tags_;
}

}  // namespace umbra::ext::usd::internal
