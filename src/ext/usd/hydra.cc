// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "hydra.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/enums.h>
#include <pxr/imaging/hd/geomSubset.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshTopology.h>
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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>
#include <vector>

#include "instancer.h"
#include "light.h"
#include "material.h"
#include "mesh.h"
#include "motion.h"
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

// The motion samples of the points of the mesh at `prim_id`, whose points at
// the frame are `frame`, over `shutter`: none when their count changes within
// it.
Motion<pxr::VtVec3fArray> SamplePoints(pxr::HdSceneDelegate& delegate,
                                       const pxr::SdfPath& prim_id,
                                       const pxr::VtValue& frame,
                                       const scene::Shutter& shutter) {
  if (!IsMotionBlurred(shutter) || !frame.IsHolding<pxr::VtVec3fArray>()) {
    return {};
  }
  const TimedSamples<pxr::VtValue> sampled =
      SamplePrimvar(delegate, prim_id, pxr::HdTokens->points, shutter);
  const std::size_t count = frame.UncheckedGet<pxr::VtVec3fArray>().size();
  TimedSamples<pxr::VtVec3fArray> samples{
      .times = sampled.times,
      .values = {},
  };
  for (const pxr::VtValue& value : sampled.values) {
    if (!value.IsHolding<pxr::VtVec3fArray>() ||
        value.UncheckedGet<pxr::VtVec3fArray>().size() != count) {
      return {.samples = {}, .outcome = MotionOutcome::kTopologyChanges};
    }
    samples.values.push_back(value.UncheckedGet<pxr::VtVec3fArray>());
  }
  return MotionOf(samples, shutter, BlendPoints);
}

// The GeomSubsets of `topology` bound to a material.
std::vector<MaterialSubset> MaterialSubsets(
    const pxr::HdMeshTopology& topology) {
  std::vector<MaterialSubset> subsets;
  for (const pxr::HdGeomSubset& subset : topology.GetGeomSubsets()) {
    if (subset.type != pxr::HdGeomSubset::TypeFaceSet ||
        subset.materialId.IsEmpty()) {
      continue;
    }
    MaterialSubset bound{.material = subset.materialId, .faces = {}};
    for (const int face : subset.indices | std::views::filter([](int face) {
                            return face >= 0;
                          })) {
      bound.faces.push_back(static_cast<std::uint32_t>(face));
    }
    subsets.push_back(std::move(bound));
  }
  return subsets;
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

// Whether the delegate syncs sprims of `type` as lights.
bool IsLightPrimType(const pxr::TfToken& type) {
  return std::ranges::find(LightPrimTypes(), type) != LightPrimTypes().end();
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

pxr::SdfPath RenderSettingsPrim::CameraPath() const {
  const pxr::VtValue& camera = GetCamera();
  return camera.IsHolding<pxr::SdfPath>() ? camera.UncheckedGet<pxr::SdfPath>()
                                          : pxr::SdfPath();
}

CameraPrim::CameraPrim(const pxr::SdfPath& prim_id,
                       core::ProgressPhase* UMBRA_NULLABLE phase)
    : pxr::HdCamera(prim_id), phase_(phase) {}

void CameraPrim::Sync(pxr::HdSceneDelegate* scene_delegate,
                      pxr::HdRenderParam* UMBRA_NULLABLE render_param,
                      pxr::HdDirtyBits* dirty_bits) {
  pxr::HdCamera::Sync(scene_delegate, render_param, dirty_bits);
  const scene::Shutter shutter{
      .open = GetShutterOpen(),
      .close = GetShutterClose(),
  };
  transform_samples_ = {.times = {0.0F}, .values = {GetTransform()}};
  if (IsMotionBlurred(shutter) && !GetId().IsEmpty()) {
    TimedSamples<pxr::GfMatrix4d> samples =
        SampleTransform(*scene_delegate, GetId(), shutter);
    if (!samples.values.empty()) {
      transform_samples_ = std::move(samples);
    }
  }
  CountSync(phase_, GetId());
}

MeshPrim::MeshPrim(const pxr::SdfPath& prim_id, core::ProgressPhase& phase,
                   double meters_per_unit, const scene::Shutter& shutter)
    : pxr::HdMesh(prim_id),
      phase_(&phase),
      meters_per_unit_(meters_per_unit),
      shutter_(&shutter) {}

pxr::HdDirtyBits MeshPrim::GetInitialDirtyBitsMask() const {
  return pxr::HdChangeTracker::AllSceneDirtyBits;
}

void MeshPrim::Sync(pxr::HdSceneDelegate* delegate,
                    pxr::HdRenderParam* UMBRA_NULLABLE /*render_param*/,
                    pxr::HdDirtyBits* dirty_bits,
                    const pxr::TfToken& /*repr*/) {
  _UpdateVisibility(delegate, dirty_bits);
  _UpdateInstancer(delegate, dirty_bits);
  if ((*dirty_bits & pxr::HdChangeTracker::DirtyMaterialId) != 0U) {
    SetMaterialId(delegate->GetMaterialId(GetId()));
  }
  if (!IsVisible()) {
    state_ = MeshState::kInvisible;
  } else {
    Convert(*delegate);
  }
  *dirty_bits &= ~pxr::HdChangeTracker::AllSceneDirtyBits;
  phase_->Add(1);
  if (state_ == MeshState::kConverted) {
    const std::size_t triangles = mesh_.triangles.size();
    const std::size_t instances = instances_.size();
    UMBRA_LOG_TRACE(kUsd, "synced {} ({} triangle{}, {} instance{})",
                    GetId().GetString(), core::FormatCount(triangles),
                    triangles == 1 ? "" : "s", core::FormatCount(instances),
                    instances == 1 ? "" : "s");
  } else {
    UMBRA_LOG_TRACE(kUsd, "synced {}", GetId().GetString());
  }
}

void MeshPrim::Convert(pxr::HdSceneDelegate& delegate) {
  SourceMesh source{
      .path = GetId().GetString(),
      .topology = GetMeshTopology(&delegate),
      .points = GetPoints(&delegate),
      .point_motion = {},
      .primvars = ReadPrimvars(delegate, GetId()),
      .sidedness = IsDoubleSided(&delegate) ? scene::Sidedness::kDoubleSided
                                            : scene::Sidedness::kSingleSided,
  };
  Motion<pxr::VtVec3fArray> motion =
      SamplePoints(delegate, GetId(), source.points, *shutter_);
  source.point_motion = std::move(motion.samples);
  motion_outcome_ = motion.outcome;
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
  ConvertDisplayColor(delegate, GetId(), mesh_.primvars);
  emission_ = internal::IsMeshLight(delegate, GetId()) ? Emission::kMeshLight
                                                       : Emission::kNone;
  material_subsets_ = MaterialSubsets(source.topology);
  // A mesh whose topology changes within the shutter has no blur at all.
  Place(delegate, motion_outcome_ == MotionOutcome::kTopologyChanges
                      ? scene::Shutter{}
                      : *shutter_);
}

void MeshPrim::Place(pxr::HdSceneDelegate& delegate,
                     const scene::Shutter& shutter) {
  Placement placement =
      PlaceInstances(delegate, GetId(), GetInstancerId(), shutter);
  motion_outcome_ = std::max(motion_outcome_, placement.outcome);
  instances_.clear();
  instances_.reserve(placement.instances.size());
  for (FlatInstance& placed : placement.instances) {
    instances_.push_back({
        .prototype = 0,
        .object_to_world =
            TransformsInMeters(placed.transforms, meters_per_unit_),
        .primvars = std::move(placed.primvars),
    });
  }
}

scene::Mesh MeshPrim::TakeMesh() { return std::move(mesh_); }

std::vector<scene::Instance> MeshPrim::TakeInstances() {
  return std::move(instances_);
}

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
  return prims_.settings.size() + prims_.cameras.size() +
         prims_.materials.size() + prims_.meshes.size() +
         prims_.instancers.size() + prims_.unsupported.size() +
         prims_.lights.size();
}

void RenderDelegate::ChooseShutter(const pxr::SdfPath& settings) {
  prims_.shutter = {};
  const auto found = prims_.settings.find(settings);
  if (found == prims_.settings.end() ||
      found->second->base().motion_blur == scene::MotionBlur::kDisabled) {
    return;
  }
  const auto camera = prims_.cameras.find(found->second->CameraPath());
  if (camera == prims_.cameras.end()) {
    return;
  }
  const scene::Shutter shutter{
      .open = camera->second->GetShutterOpen(),
      .close = camera->second->GetShutterClose(),
  };
  if (IsMotionBlurred(shutter)) {
    prims_.shutter = shutter;
  }
}

void RenderDelegate::PlaceLights(const pxr::TfTokenVector& render_tags) {
  for (const auto& [path, light] : prims_.lights) {
    light->Place(prims_.shutter, render_tags);
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
  static const pxr::TfTokenVector kTypes = [] {
    pxr::TfTokenVector types = {
        pxr::HdPrimTypeTokens->camera,
        pxr::HdPrimTypeTokens->material,
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
    pxr::HdRenderIndex* /*index*/,
    const pxr::HdRprimCollection& /*collection*/) {
  // umbra renders outside Hydra: it has no render pass.
  return nullptr;
}

pxr::HdInstancer* RenderDelegate::CreateInstancer(
    pxr::HdSceneDelegate* delegate, const pxr::SdfPath& prim_id) {
  auto instancer = std::make_unique<InstancerPrim>(delegate, prim_id, *phase_,
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
    auto mesh = std::make_unique<MeshPrim>(prim_id, *phase_, meters_per_unit_,
                                           prims_.shutter);
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
  if (IsLightPrimType(type_id)) {
    auto light =
        std::make_unique<LightPrim>(type_id, prim_id, phase_, meters_per_unit_);
    prims_.lights[prim_id] = light.get();
    return Release<pxr::HdSprim>(std::move(light));
  }
  if (type_id == pxr::HdPrimTypeTokens->material) {
    auto material = std::make_unique<MaterialPrim>(prim_id, phase_);
    prims_.materials[prim_id] = material.get();
    return Release<pxr::HdSprim>(std::move(material));
  }
  if (type_id != pxr::HdPrimTypeTokens->camera) {
    return nullptr;
  }
  auto camera = std::make_unique<CameraPrim>(prim_id, phase_);
  prims_.cameras[prim_id] = camera.get();
  return Release<pxr::HdSprim>(std::move(camera));
}

pxr::HdSprim* UMBRA_NULLABLE
RenderDelegate::CreateFallbackSprim(const pxr::TfToken& type_id) {
  if (IsLightPrimType(type_id)) {
    return Release<pxr::HdSprim>(std::make_unique<LightPrim>(
        type_id, pxr::SdfPath::EmptyPath(), nullptr, meters_per_unit_));
  }
  if (type_id == pxr::HdPrimTypeTokens->material) {
    return Release<pxr::HdSprim>(
        std::make_unique<MaterialPrim>(pxr::SdfPath::EmptyPath(), nullptr));
  }
  if (type_id != pxr::HdPrimTypeTokens->camera) {
    return nullptr;
  }
  return Release<pxr::HdSprim>(
      std::make_unique<CameraPrim>(pxr::SdfPath::EmptyPath(), nullptr));
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
  for (const auto& [path, instancer] : prims_.instancers) {
    if (!instancer->IsSynced()) {
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

SyncTask::SyncTask(RenderDelegate& delegate, pxr::SdfPath settings)
    : pxr::HdTask(SyncTaskPath()),
      delegate_(&delegate),
      settings_(std::move(settings)) {}

void SyncTask::Sync(pxr::HdSceneDelegate* UMBRA_NULLABLE /*delegate*/,
                    pxr::HdTaskContext* /*context*/,
                    pxr::HdDirtyBits* dirty_bits) {
  // The sprims (the render settings, the cameras, the lights) are synced
  // before the tasks, and the rprims after.
  render_tags_ = delegate_->RenderTags(settings_);
  delegate_->ChooseShutter(settings_);
  delegate_->PlaceLights(render_tags_);
  *dirty_bits = pxr::HdChangeTracker::Clean;
}

void SyncTask::Prepare(pxr::HdTaskContext* /*context*/,
                       pxr::HdRenderIndex* /*index*/) {}

void SyncTask::Execute(pxr::HdTaskContext* /*context*/) {}

const pxr::TfTokenVector& SyncTask::GetRenderTags() const {
  return render_tags_;
}

}  // namespace umbra::ext::usd::internal
