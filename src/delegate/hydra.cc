// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "hydra.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range1f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/base/work/loops.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/dataSourceLocator.h>
#include <pxr/imaging/hd/dirtyBitsTranslator.h>
#include <pxr/imaging/hd/enums.h>
#include <pxr/imaging/hd/extComputation.h>
#include <pxr/imaging/hd/extComputationUtils.h>
#include <pxr/imaging/hd/geomSubset.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/renderSettings.h>
#include <pxr/imaging/hd/repr.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/rprimCollection.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/sprim.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/timeSampleArray.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hdsi/renderSettingsFilteringSceneIndex.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usdRender/tokens.h>
#include <pxr/usdImaging/usdImaging/usdRenderSettingsSchema.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "denoiser.h"
#include "dicing.h"
#include "host_copy.h"
#include "ies_tilt.h"
#include "instancer.h"
#include "light.h"
#include "material.h"
#include "mesh.h"
#include "motion.h"
#include "motion_samples.h"
#include "render_pass.h"
#include "sync_counter.h"
#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/delegate/session.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/device/image.h"

namespace umbra::delegate::internal {
namespace {

// Counts the sync of the prim at `prim_id` into `counter`, for a prim that is
// not a fallback.
void CountSync(SyncCounter* UMBRA_NULLABLE counter,
               const pxr::SdfPath& prim_id) {
  if (counter == nullptr) {
    return;
  }
  counter->Add(1);
  UMBRA_LOG_TRACE(kUsd, "synced {}", prim_id.GetString());
}

// Gives `bits` kDirtyEmission when `locators` hold a mesh's light data
// source, which MeshLightAPI authors.
void EmissionBitsOf(const pxr::HdDataSourceLocatorSet& locators,
                    pxr::HdDirtyBits* bits) {
  if (locators.Intersects(pxr::HdLightSchema::GetDefaultLocator())) {
    *bits |= kDirtyEmission;
  }
}

// Gives `locators` a mesh's light data source when `bits` hold
// kDirtyEmission.
void EmissionLocatorsOf(pxr::HdDirtyBits bits,
                        pxr::HdDataSourceLocatorSet* locators) {
  if ((bits & kDirtyEmission) != 0U) {
    locators->append(pxr::HdLightSchema::GetDefaultLocator());
  }
}

// Has Hydra mark a mesh kDirtyEmission when its light data source changes:
// no dirty bit of Hydra's names it. The mapping is Hydra's, process-wide,
// for every mesh rprim, and comes on top of Hydra's own.
void MapEmissionToDirtyBit() {
  static std::once_flag once;
  std::call_once(once, [] {
    pxr::HdDirtyBitsTranslator::RegisterTranslatorsForCustomRprimType(
        pxr::HdPrimTypeTokens->mesh, EmissionBitsOf, EmissionLocatorsOf);
  });
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
    base.motion_blur = MotionBlur::kDisabled;
  }
  if (ValueOr(schema.GetDisableDepthOfField(), false)) {
    base.depth_of_field = DepthOfField::kDisabled;
  }
  return base;
}

// The name of the dicing rate on a RenderSettings prim.
const pxr::TfToken& DicingRateName() {
  static const pxr::TfToken kName("umbra:dicingRate");
  return kName;
}

// umbra:dicingRate, from the prim's UsdRender data source in the terminal
// scene index.
DicingRate ReadDicingRate(const pxr::HdContainerDataSourceHandle& prim) {
  const pxr::HdContainerDataSourceHandle settings =
      pxr::UsdImagingUsdRenderSettingsSchema::GetFromParent(prim)
          .GetNamespacedSettings();
  const auto source = pxr::HdSampledDataSource::Cast(
      settings != nullptr ? settings->Get(DicingRateName()) : nullptr);
  if (source == nullptr) {
    return {};
  }
  const pxr::VtValue value = source->GetValue(0.0F);
  double pixels = 0.0;
  if (value.IsHolding<float>()) {
    pixels = static_cast<double>(value.UncheckedGet<float>());
  } else if (value.IsHolding<double>()) {
    pixels = value.UncheckedGet<double>();
  }
  if (!(pixels > 0.0) || !std::isfinite(pixels)) {
    return {
        .pixels = 1.0,
        .invalid = "umbra:dicingRate must be a float above 0",
    };
  }
  return {.pixels = pixels, .invalid = {}};
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
          .element_size = descriptor.elementSize,
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

// The ext computation primvar that gives the points of the mesh at
// `prim_id`, as usdSkelImaging gives those of a skinned mesh: none when its
// points are a plain primvar.
std::optional<pxr::HdExtComputationPrimvarDescriptor> ComputedPointsOf(
    pxr::HdSceneDelegate& delegate, const pxr::SdfPath& prim_id) {
  for (pxr::HdExtComputationPrimvarDescriptor& descriptor :
       delegate.GetExtComputationPrimvarDescriptors(
           prim_id, pxr::HdInterpolationVertex)) {
    if (descriptor.name == pxr::HdTokens->points) {
      return std::move(descriptor);
    }
  }
  return std::nullopt;
}

// The points of the mesh at `prim_id` at the frame: the output of `computed`
// when they are computed.
pxr::VtValue PointsAtFrame(
    pxr::HdSceneDelegate& delegate, const pxr::SdfPath& prim_id,
    const std::optional<pxr::HdExtComputationPrimvarDescriptor>& computed) {
  if (!computed.has_value()) {
    return delegate.Get(prim_id, pxr::HdTokens->points);
  }
  pxr::HdExtComputationUtils::ValueStore values =
      pxr::HdExtComputationUtils::GetComputedPrimvarValues({*computed},
                                                           &delegate);
  const auto found = values.find(pxr::HdTokens->points);
  return found != values.end() ? std::move(found->second) : pxr::VtValue();
}

// The samples of the output of `computed` over `shutter`, which spans an
// interval: one at each time its inputs are sampled at.
TimedSamples<pxr::VtValue> SampleComputedPoints(
    pxr::HdSceneDelegate& delegate,
    const pxr::HdExtComputationPrimvarDescriptor& computed,
    const host::Shutter& shutter) {
  using Utils = pxr::HdExtComputationUtils;
  Utils::SampledValueStore<kMaxMotionSamples> values;
  // Every time is computed, none dropped: more than kMaxMotionSamples are
  // resampled down, as a plain primvar's are.
  Utils::SampleComputedPrimvarValues<kMaxMotionSamples>(
      {computed}, &delegate, static_cast<float>(shutter.open),
      static_cast<float>(shutter.close),
      std::numeric_limits<std::size_t>::max(), &values);
  const auto found = values.find(pxr::HdTokens->points);
  if (found == values.end()) {
    return {};
  }
  const pxr::HdTimeSampleArray<pxr::VtValue, kMaxMotionSamples>& sampled =
      found->second;
  TimedSamples<pxr::VtValue> samples;
  samples.times.reserve(sampled.count);
  samples.values.reserve(sampled.count);
  for (const auto& [time, value] :
       std::views::zip(sampled.times, sampled.values) |
           std::views::take(sampled.count)) {
    samples.times.push_back(time);
    samples.values.push_back(value);
  }
  return samples;
}

// The motion samples of the points of the mesh at `prim_id`, whose points at
// the frame are `frame`, over `shutter`: none when their count changes within
// it. They are sampled from the output of `computed` when they are computed.
Motion<pxr::VtVec3fArray> SamplePoints(
    pxr::HdSceneDelegate& delegate, const pxr::SdfPath& prim_id,
    const std::optional<pxr::HdExtComputationPrimvarDescriptor>& computed,
    const pxr::VtValue& frame, const host::Shutter& shutter) {
  if (!IsMotionBlurred(shutter) || !frame.IsHolding<pxr::VtVec3fArray>()) {
    return {};
  }
  const TimedSamples<pxr::VtValue> sampled =
      computed.has_value()
          ? SampleComputedPoints(delegate, *computed, shutter)
          : SamplePrimvar(delegate, prim_id, pxr::HdTokens->points, shutter);
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

// The path of the task, which is not in the render index, and under which
// it names its render buffers.
const pxr::SdfPath& TaskPath() {
  static const pxr::SdfPath kPath("/__umbraRenderTask");
  return kPath;
}

}  // namespace

AspectRatioConformPolicy ConformPolicy(const pxr::TfToken& token) {
  using enum AspectRatioConformPolicy;
  if (token == pxr::UsdRenderTokens->cropAperture) {
    return kCropAperture;
  }
  if (token == pxr::UsdRenderTokens->adjustApertureWidth) {
    return kAdjustApertureWidth;
  }
  if (token == pxr::UsdRenderTokens->adjustApertureHeight) {
    return kAdjustApertureHeight;
  }
  if (token == pxr::UsdRenderTokens->adjustPixelAspectRatio) {
    return kAdjustPixelAspectRatio;
  }
  return kExpandAperture;
}

RenderSettingsPrim::RenderSettingsPrim(const pxr::SdfPath& prim_id,
                                       SyncCounter* UMBRA_NULLABLE counter)
    : pxr::HdRenderSettings(prim_id), counter_(counter) {}

void RenderSettingsPrim::_Sync(
    pxr::HdSceneDelegate* scene_delegate,
    pxr::HdRenderParam* UMBRA_NULLABLE /*render_param*/,
    const pxr::HdDirtyBits* /*dirty_bits*/) {
  const pxr::HdSceneIndexBaseRefPtr terminal =
      scene_delegate->GetRenderIndex().GetTerminalSceneIndex();
  if (terminal != nullptr) {
    const pxr::HdContainerDataSourceHandle prim =
        terminal->GetPrim(GetId()).dataSource;
    base_ = ReadSettingsBase(prim);
    dicing_rate_ = ReadDicingRate(prim);
  }
  CountSync(counter_, GetId());
}

pxr::SdfPath RenderSettingsPrim::CameraPath() const {
  const pxr::VtValue& camera = GetCamera();
  return camera.IsHolding<pxr::SdfPath>() ? camera.UncheckedGet<pxr::SdfPath>()
                                          : pxr::SdfPath();
}

CameraPrim::CameraPrim(const pxr::SdfPath& prim_id,
                       SyncCounter* UMBRA_NULLABLE counter,
                       double meters_per_unit)
    : pxr::HdCamera(prim_id),
      counter_(counter),
      meters_per_unit_(meters_per_unit) {}

void CameraPrim::Sync(pxr::HdSceneDelegate* scene_delegate,
                      pxr::HdRenderParam* UMBRA_NULLABLE render_param,
                      pxr::HdDirtyBits* dirty_bits) {
  pxr::HdCamera::Sync(scene_delegate, render_param, dirty_bits);
  const host::Shutter shutter{
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
  changed_ = Changed::kYes;
  CountSync(counter_, GetId());
}

host::Camera CameraPrim::Converted(
    std::span<const pxr::GfMatrix4d> camera_to_world) const {
  // Lengths are in scene units, the apertures and focal length too once
  // Hydra has read them.
  const auto meters = [this](float length) {
    return static_cast<double>(length) * meters_per_unit_;
  };
  const pxr::GfRange1f& clipping = GetClippingRange();
  return {
      .path = GetId().GetString(),
      .projection = GetProjection() == pxr::HdCamera::Orthographic
                        ? host::Projection::kOrthographic
                        : host::Projection::kPerspective,
      .camera_to_world = TransformsInMeters(camera_to_world, meters_per_unit_),
      .focal_length = meters(GetFocalLength()),
      .aperture =
          {
              meters(GetHorizontalAperture()),
              meters(GetVerticalAperture()),
          },
      .aperture_offset =
          {
              meters(GetHorizontalApertureOffset()),
              meters(GetVerticalApertureOffset()),
          },
      .clipping_range = {meters(clipping.GetMin()), meters(clipping.GetMax())},
      .f_stop = static_cast<double>(GetFStop()),
      .focus_distance = meters(GetFocusDistance()),
      .exposure_scale = GetLinearExposureScale(),
  };
}

void CameraPrim::Place(const host::Shutter& shutter) {
  const SampledTransform camera_to_world =
      ResampleTransform(transform_samples_, GetTransform(), shutter);
  motion_outcome_ = camera_to_world.outcome;
  camera_ = Converted(camera_to_world.motion);
}

host::Camera CameraPrim::Sampled(const host::Shutter& shutter) const {
  return Converted(
      ResampleTransform(transform_samples_, GetTransform(), shutter).motion);
}

host::Camera CameraPrim::AtFrame() const {
  const std::array<pxr::GfMatrix4d, 1> at_frame{GetTransform()};
  return Converted(at_frame);
}

void CameraPrim::FreeHostCopy() {
  camera_ = {};
  changed_ = Changed::kNo;
}

MeshPrim::MeshPrim(const pxr::SdfPath& prim_id, SyncCounter& counter,
                   double meters_per_unit, const host::Shutter& shutter,
                   const DicingView& dicing)
    : pxr::HdMesh(prim_id),
      counter_(&counter),
      meters_per_unit_(meters_per_unit),
      shutter_(&shutter),
      dicing_(&dicing) {}

pxr::HdDirtyBits MeshPrim::GetInitialDirtyBitsMask() const {
  return pxr::HdChangeTracker::AllSceneDirtyBits | kDirtyEmission;
}

void MeshPrim::Sync(pxr::HdSceneDelegate* delegate,
                    pxr::HdRenderParam* UMBRA_NULLABLE /*render_param*/,
                    pxr::HdDirtyBits* dirty_bits,
                    const pxr::TfToken& /*repr*/) {
  const pxr::HdDirtyBits bits = *dirty_bits;
  delegate_ = delegate;
  _UpdateVisibility(delegate, dirty_bits);
  _UpdateInstancer(delegate, dirty_bits);
  if ((bits & pxr::HdChangeTracker::DirtyMaterialId) != 0U) {
    SetMaterialId(delegate->GetMaterialId(GetId()));
  }
  const bool placed = state_ == MeshState::kConverted && instance_count_ > 0;
  if (!IsVisible()) {
    state_ = MeshState::kInvisible;
    instance_count_ = 0;
  } else if (placed) {
    Update(*delegate, bits);
  } else {
    Convert(*delegate);
  }
  *dirty_bits &= ~(pxr::HdChangeTracker::AllSceneDirtyBits | kDirtyEmission);
  counter_->Add(1);
  LogSync();
}

void MeshPrim::LogSync() const {
  if (state_ != MeshState::kConverted || changes_ != kWholeMesh) {
    UMBRA_LOG_TRACE(kUsd, "synced {}", GetId().GetString());
    return;
  }
  const std::size_t triangles = mesh_.triangles.size();
  UMBRA_LOG_TRACE(kUsd, "synced {} ({} triangle{}, {} instance{})",
                  GetId().GetString(), core::FormatCount(triangles),
                  triangles == 1 ? "" : "s", core::FormatCount(instance_count_),
                  instance_count_ == 1 ? "" : "s");
}

void MeshPrim::LogRefined() const {
  const std::size_t triangles = mesh_.triangles.size();
  UMBRA_LOG_DEBUG(kUsd,
                  "{}: {} subdivision refined into {} triangle{}, the "
                  "longest edge {:.3g} pixels, for a dicing rate of {}",
                  GetId().GetString(), subdivision_scheme_,
                  core::FormatCount(triangles), triangles == 1 ? "" : "s",
                  longest_edge_, dicing_->rate);
}

void MeshPrim::Convert(pxr::HdSceneDelegate& delegate) {
  ConvertGeometry(delegate);
  if (state_ != MeshState::kConverted) {
    return;
  }
  // A subdivided mesh is placed before it is refined.
  if (subdivision_scheme_.empty()) {
    Place(delegate);
  }
  changes_ = kWholeMesh;
}

namespace {

// The dirty bits of a mesh's placement.
constexpr pxr::HdDirtyBits kPlacementBits =
    pxr::HdChangeTracker::DirtyTransform |
    pxr::HdChangeTracker::DirtyInstancer |
    pxr::HdChangeTracker::DirtyInstanceIndex;

// The dirty bits that have a subdivided mesh refined again.
constexpr pxr::HdDirtyBits kRefineBits =
    kPlacementBits | pxr::HdChangeTracker::DirtyTopology |
    pxr::HdChangeTracker::DirtySubdivTags | pxr::HdChangeTracker::DirtyPoints |
    pxr::HdChangeTracker::DirtyPrimvar | pxr::HdChangeTracker::DirtyDoubleSided;

}  // namespace

void MeshPrim::Refine(pxr::HdSceneDelegate& delegate, pxr::HdDirtyBits bits) {
  using device::ChangeFlags;
  const MotionOutcome outcome = points_outcome_;
  ConvertGeometry(delegate);
  if (state_ != MeshState::kConverted) {
    return;
  }
  ChangeFlags flags =
      ChangeFlags::kTopology | ChangeFlags::kPrimvars | ChangeFlags::kMaterials;
  if ((bits & kPlacementBits) != 0U || points_outcome_ != outcome) {
    flags = flags | ChangeFlags::kPlacement;
  }
  if ((bits & kDirtyEmission) != 0U) {
    flags = flags | ChangeFlags::kEmission;
  }
  changes_ = changes_ | flags;
  KeepChanged();
}

void MeshPrim::RefineForView() {
  if (delegate_ == nullptr || !IsRefined() || instance_count_ == 0) {
    return;
  }
  // Hydra syncs it after the tasks, which refines it.
  const pxr::HdDirtyBits dirty =
      delegate_->GetRenderIndex().GetChangeTracker().GetRprimDirtyBits(GetId());
  if ((dirty & (kRefineBits | pxr::HdChangeTracker::DirtyVisibility)) != 0U) {
    return;
  }
  Refine(*delegate_, pxr::HdChangeTracker::Clean);
}

void MeshPrim::Update(pxr::HdSceneDelegate& delegate, pxr::HdDirtyBits bits) {
  if (!subdivision_scheme_.empty() && (bits & kRefineBits) != 0U) {
    Refine(delegate, bits);
    return;
  }
  const MotionOutcome outcome = points_outcome_;
  const std::optional<device::ChangeFlags> geometry =
      UpdateGeometry(delegate, bits);
  if (!geometry.has_value()) {
    return;
  }
  device::ChangeFlags flags = *geometry;
  // Points whose topology changes within the shutter leave the instances
  // without blur, and points that stop changing give it back.
  if ((bits & kPlacementBits) != 0U || points_outcome_ != outcome) {
    Place(delegate);
    flags = flags | device::ChangeFlags::kPlacement;
  }
  if ((bits & kDirtyEmission) != 0U) {
    ReadEmission(delegate);
    flags = flags | device::ChangeFlags::kEmission;
  }
  changes_ = changes_ | flags;
  KeepChanged();
}

device::ChangeFlags MeshPrim::DirtiedOf(pxr::HdDirtyBits bits) const {
  using Tracker = pxr::HdChangeTracker;
  using device::ChangeFlags;
  ChangeFlags flags = ChangeFlags::kNone;
  if ((bits & (Tracker::DirtyTopology | Tracker::DirtyDoubleSided)) != 0U) {
    flags = flags | ChangeFlags::kTopology;
  }
  if ((bits & Tracker::DirtyPoints) != 0U) {
    flags = flags | ChangeFlags::kPoints;
    // Computed normals follow the points.
    if (normals_ == NormalsSource::kComputed) {
      flags = flags | ChangeFlags::kPrimvars;
    }
  }
  if ((bits & (Tracker::DirtyPrimvar | Tracker::DirtyNormals)) != 0U) {
    flags = flags | ChangeFlags::kPrimvars;
  }
  if ((bits & Tracker::DirtyMaterialId) != 0U) {
    flags = flags | ChangeFlags::kMaterials;
  }
  return flags;
}

std::optional<device::ChangeFlags> MeshPrim::UpdateGeometry(
    pxr::HdSceneDelegate& delegate, pxr::HdDirtyBits bits) {
  using device::ChangeFlags;
  using device::HasAny;
  const ChangeFlags dirtied = DirtiedOf(bits);
  // The materials of the triangles of subsets need the triangles.
  const bool whole =
      HasAny(dirtied, ChangeFlags::kTopology | ChangeFlags::kPrimvars) ||
      (HasAny(dirtied, ChangeFlags::kMaterials) && !material_subsets_.empty());
  if (whole) {
    return ReconvertGeometry(delegate, dirtied);
  }
  const ChangeFlags materials = HasAny(dirtied, ChangeFlags::kMaterials)
                                    ? ChangeFlags::kMaterials
                                    : ChangeFlags::kNone;
  if (!HasAny(dirtied, ChangeFlags::kPoints)) {
    return materials;
  }
  if (ConvertPointsAlone(delegate) == PointsConversion::kConverted) {
    return materials | ChangeFlags::kPoints;
  }
  return ReconvertGeometry(delegate, dirtied | ChangeFlags::kTopology);
}

std::optional<device::ChangeFlags> MeshPrim::ReconvertGeometry(
    pxr::HdSceneDelegate& delegate, device::ChangeFlags dirtied) {
  using device::ChangeFlags;
  const std::size_t point_count = point_count_;
  const DisplayColor display_color = display_color_;
  ConvertGeometry(delegate);
  if (state_ != MeshState::kConverted) {
    return std::nullopt;
  }
  if (device::HasAny(dirtied, ChangeFlags::kTopology) ||
      point_count_ != point_count) {
    return ChangeFlags::kTopology | ChangeFlags::kPrimvars |
           ChangeFlags::kMaterials;
  }
  // A mesh with no material of its own gets the display color material
  // when it has a displayColor, and the fallback one otherwise.
  if (display_color_ != display_color) {
    return dirtied | ChangeFlags::kMaterials;
  }
  return dirtied;
}

SourceMesh MeshPrim::ReadSource(pxr::HdSceneDelegate& delegate) {
  const std::optional<pxr::HdExtComputationPrimvarDescriptor> computed =
      ComputedPointsOf(delegate, GetId());
  SourceMesh source{
      .path = GetId().GetString(),
      .topology = GetMeshTopology(&delegate),
      .subdiv_tags = GetSubdivTags(&delegate),
      .points = PointsAtFrame(delegate, GetId(), computed),
      .point_motion = {},
      .primvars = ReadPrimvars(delegate, GetId()),
      .sidedness = IsDoubleSided(&delegate) ? host::Sidedness::kDoubleSided
                                            : host::Sidedness::kSingleSided,
  };
  Motion<pxr::VtVec3fArray> motion =
      SamplePoints(delegate, GetId(), computed, source.points, *shutter_);
  source.point_motion = std::move(motion.samples);
  points_outcome_ = motion.outcome;
  return source;
}

void MeshPrim::ReadDisplayColor(pxr::HdSceneDelegate& delegate) {
  ConvertDisplayColor(delegate, GetId(), mesh_.primvars);
  display_color_ = std::ranges::any_of(mesh_.primvars,
                                       [](const host::Primvar& primvar) {
                                         return primvar.name == kDisplayColor;
                                       })
                       ? DisplayColor::kPresent
                       : DisplayColor::kAbsent;
}

void MeshPrim::ConvertGeometry(pxr::HdSceneDelegate& delegate) {
  const SourceMesh source = ReadSource(delegate);
  subdivision_scheme_ = IsSubdivided(source.topology)
                            ? source.topology.GetScheme().GetString()
                            : std::string();
  if (!subdivision_scheme_.empty()) {
    Place(delegate);
  }
  auto converted =
      subdivision_scheme_.empty()
          ? ConvertMesh(source, meters_per_unit_)
          : RefineMesh(source, meters_per_unit_, *dicing_, instances_);
  if (!converted.has_value()) {
    state_ = MeshState::kInvalid;
    instance_count_ = 0;
    invalid_reason_ = std::move(converted.error());
    return;
  }
  state_ = MeshState::kConverted;
  mesh_ = std::move(converted->mesh);
  normals_ = converted->normals;
  longest_edge_ = converted->longest_edge;
  if (!subdivision_scheme_.empty()) {
    LogRefined();
  }
  point_count_ = mesh_.points.front().size();
  ReadDisplayColor(delegate);
  ReadEmission(delegate);
  material_subsets_ = MaterialSubsets(source.topology);
  AssignTriangleMaterials();
}

MeshPrim::PointsConversion MeshPrim::ConvertPointsAlone(
    pxr::HdSceneDelegate& delegate) {
  const std::optional<pxr::HdExtComputationPrimvarDescriptor> computed =
      ComputedPointsOf(delegate, GetId());
  const pxr::VtValue frame = PointsAtFrame(delegate, GetId(), computed);
  Motion<pxr::VtVec3fArray> motion =
      SamplePoints(delegate, GetId(), computed, frame, *shutter_);
  if (motion.outcome != points_outcome_) {
    return PointsConversion::kWholeNeeded;
  }
  auto points = ConvertPoints(frame, motion.samples, meters_per_unit_);
  if (!points.has_value() || points->front().size() != point_count_) {
    return PointsConversion::kWholeNeeded;
  }
  mesh_.points = std::move(*points);
  return PointsConversion::kConverted;
}

namespace {

// Gives the faces of `subset` the material `index` in `face_materials`,
// those out of range left out.
void MarkFaces(const MaterialSubset& subset, std::uint32_t index,
               std::vector<std::uint32_t>& face_materials) {
  for (const std::uint32_t face : subset.faces) {
    if (face < face_materials.size()) {
      face_materials.at(face) = index;
    }
  }
}

}  // namespace

void MeshPrim::AssignTriangleMaterials() {
  mesh_.triangle_materials.clear();
  if (material_subsets_.empty()) {
    return;
  }
  // The material of each source face, an index into the mesh's materials.
  std::vector<std::uint32_t> face_materials(mesh_.face_vertex_counts.size(), 0);
  std::uint32_t index = 0;
  for (const MaterialSubset& subset : material_subsets_) {
    MarkFaces(subset, ++index, face_materials);
  }
  mesh_.triangle_materials.reserve(mesh_.source_faces.size());
  for (const std::uint32_t face : mesh_.source_faces) {
    mesh_.triangle_materials.push_back(face_materials.at(face));
  }
}

void MeshPrim::Place(pxr::HdSceneDelegate& delegate) {
  // A mesh whose topology changes within the shutter has no blur at all.
  const host::Shutter shutter =
      points_outcome_ == MotionOutcome::kTopologyChanges ? host::Shutter{}
                                                         : *shutter_;
  Placement placement =
      PlaceInstances(delegate, GetId(), GetInstancerId(), shutter);
  placement_outcome_ = placement.outcome;
  instances_.clear();
  instances_.reserve(placement.instances.size());
  for (FlatInstance& placed : placement.instances) {
    instances_.push_back({
        .object_to_world =
            TransformsInMeters(placed.transforms, meters_per_unit_),
        .primvars = std::move(placed.primvars),
    });
  }
  instance_count_ = instances_.size();
}

void MeshPrim::ReadEmission(pxr::HdSceneDelegate& delegate) {
  emission_notes_.clear();
  mesh_.emission = ReadMeshLight(delegate, GetId(), emission_notes_);
}

void MeshPrim::KeepChanged() {
  using device::ChangeFlags;
  using device::HasAny;
  if (!HasAny(changes_, ChangeFlags::kPoints | ChangeFlags::kTopology)) {
    mesh_.points = {};
  }
  if (!HasAny(changes_, ChangeFlags::kTopology)) {
    mesh_.triangles = {};
    mesh_.source_faces = {};
    mesh_.face_vertex_counts = {};
  }
  if (!HasAny(changes_, ChangeFlags::kPrimvars)) {
    mesh_.normals = {};
    mesh_.primvars = {};
  }
  if (!HasAny(changes_, ChangeFlags::kMaterials)) {
    mesh_.triangle_materials = {};
  }
  if (!HasAny(changes_, ChangeFlags::kPlacement)) {
    instances_ = {};
  }
  if (!HasAny(changes_, ChangeFlags::kEmission)) {
    mesh_.emission.reset();
  }
}

void MeshPrim::FreeHostCopy() {
  // Its path stays: an update names its prim.
  std::string path = std::move(mesh_.path);
  mesh_ = {};
  mesh_.path = std::move(path);
  instances_ = {};
  changes_ = device::ChangeFlags::kNone;
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
                                 SyncCounter& counter)
    : pxr::HdRprim(prim_id), type_(std::move(type_id)), counter_(&counter) {}

pxr::HdDirtyBits UnsupportedPrim::GetInitialDirtyBitsMask() const {
  return pxr::HdChangeTracker::AllSceneDirtyBits;
}

void UnsupportedPrim::Sync(pxr::HdSceneDelegate* /*delegate*/,
                           pxr::HdRenderParam* UMBRA_NULLABLE /*render_param*/,
                           pxr::HdDirtyBits* dirty_bits,
                           const pxr::TfToken& /*repr*/) {
  *dirty_bits &= ~pxr::HdChangeTracker::AllSceneDirtyBits;
  synced_ = Synced::kSynced;
  CountSync(counter_, GetId());
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

namespace {

// The images of the products of `settings` that see through its camera,
// or the settings' own when none does.
std::vector<device::Image> ImagesOf(const RenderSettingsPrim& settings) {
  std::vector<device::Image> images;
  for (const pxr::HdRenderSettings::RenderProduct& product :
       settings.GetRenderProducts()) {
    if (product.cameraPath == settings.CameraPath()) {
      images.push_back({
          .resolution = Components<std::int32_t, 2>(product.resolution),
          .window = {},
          .pixel_aspect_ratio = product.pixelAspectRatio,
          .aspect_ratio_conform_policy =
              ConformPolicy(product.aspectRatioConformPolicy),
      });
    }
  }
  if (images.empty()) {
    const SettingsBase& base = settings.base();
    images.push_back({
        .resolution = base.resolution,
        .window = {},
        .pixel_aspect_ratio = base.pixel_aspect_ratio,
        .aspect_ratio_conform_policy =
            ConformPolicy(base.aspect_ratio_conform_policy),
    });
  }
  return images;
}

// The pixels per unit of length of the aperture of `camera` on `image`:
// the larger of its width's and its height's.
double PixelsPerAperture(const host::Camera& camera,
                         const device::Image& image) {
  const device::ConformedAperture conformed =
      device::ConformAperture(camera.aperture, image.resolution,
                              static_cast<double>(image.pixel_aspect_ratio),
                              image.aspect_ratio_conform_policy);
  double pixels = 0.0;
  for (std::size_t axis = 0; axis < 2; ++axis) {
    const double aperture = conformed.aperture.at(axis);
    if (aperture > 0.0) {
      pixels = std::max(
          pixels, static_cast<double>(image.resolution.at(axis)) / aperture);
    }
  }
  return pixels;
}

// How the images of `settings` scale a length seen through `camera`.
subdivision::ImageScale ImageScaleOf(const host::Camera& camera,
                                     const RenderSettingsPrim& settings) {
  double pixels = 0.0;
  for (const device::Image& image : ImagesOf(settings)) {
    pixels = std::max(pixels, PixelsPerAperture(camera, image));
  }
  // A perspective camera's aperture spans its focal length per radian.
  const bool perspective = camera.projection == host::Projection::kPerspective;
  return {
      .projection = camera.projection,
      .pixels_per_unit = perspective ? pixels * camera.focal_length : pixels,
      .near = camera.clipping_range.at(0),
  };
}

}  // namespace

DicingView RenderDelegate::DicingOf(const pxr::SdfPath& settings) const {
  const auto found = prims_.settings.find(settings);
  if (found == prims_.settings.end()) {
    return {};
  }
  const RenderSettingsPrim& prim = *found->second;
  DicingView view{
      .scale = {},
      .eyes = {},
      .rate = prim.dicing_rate().pixels,
  };
  const auto camera = prims_.cameras.find(prim.CameraPath());
  if (camera == prims_.cameras.end()) {
    return view;
  }
  const host::Camera sampled = camera->second->Sampled(prims_.shutter);
  for (const host::Matrix& camera_to_world : sampled.camera_to_world) {
    view.eyes.push_back({
        camera_to_world.at(12),
        camera_to_world.at(13),
        camera_to_world.at(14),
    });
  }
  view.scale = ImageScaleOf(sampled, prim);
  return view;
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

namespace {

// Whether `product` has a var of `source`.
bool HasVar(const RenderProduct& product, RenderVarSource source) {
  return std::ranges::contains(product.vars, source, &RenderVar::source);
}

// The inputs of the denoise of `product` that it has no var of: none when it
// has no denoisedColor var.
std::vector<RenderVarSource> InternalInputs(const RenderProduct& product) {
  std::vector<RenderVarSource> inputs;
  if (!HasVar(product, RenderVarSource::kDenoisedColor)) {
    return inputs;
  }
  static constexpr std::array<RenderVarSource, 3> kInputs = {
      RenderVarSource::kColor,
      RenderVarSource::kAlbedo,
      RenderVarSource::kNormal,
  };
  for (const RenderVarSource source : kInputs) {
    if (!HasVar(product, source)) {
      inputs.push_back(source);
    }
  }
  return inputs;
}

// A render buffer of `product`, the `index`th, the `slot`th of its buffers,
// with `channels` channels over its data window, set to 0, for `what`.
std::expected<std::unique_ptr<RenderBuffer>, core::Error> CreateBuffer(
    const RenderProduct& product, std::size_t index, std::size_t slot,
    std::uint32_t channels, std::string_view what) {
  auto buffer = std::make_unique<RenderBuffer>(TaskPath().AppendChild(
      pxr::TfToken(std::format("product{}_{}", index, slot))));
  if (!buffer->Allocate(
          pxr::GfVec3i(product.window.width(), product.window.height(), 1),
          FloatFormat(channels), false)) {
    return std::unexpected(
        core::Error(core::ErrorCode::kInternal,
                    std::format("{}: no render buffer of {} channels for {}",
                                product.file, channels, what)));
  }
  return buffer;
}

// The render buffers of `product`, the `index`th, sized to its data window
// and set to 0: those of its vars, then those of the inputs of its denoise
// it has no var of, RGBA for the color and RGB for the albedo and the
// normal.
std::expected<ProductBuffers, core::Error> CreateBuffers(
    const RenderProduct& product, std::size_t index) {
  const std::vector<RenderVarSource> internal = InternalInputs(product);
  ProductBuffers buffers;
  buffers.reserve(product.vars.size() + internal.size());
  for (const RenderVar& var : product.vars) {
    auto buffer =
        CreateBuffer(product, index, buffers.size(), var.channels, var.name);
    if (!buffer.has_value()) {
      return std::unexpected(std::move(buffer.error()));
    }
    buffers.push_back(*std::move(buffer));
  }
  for (const RenderVarSource source : internal) {
    const std::uint32_t channels = source == RenderVarSource::kColor ? 4 : 3;
    auto buffer = CreateBuffer(product, index, buffers.size(), channels,
                               AovName(source).GetString());
    if (!buffer.has_value()) {
      return std::unexpected(std::move(buffer.error()));
    }
    buffers.push_back(*std::move(buffer));
  }
  return buffers;
}

// The AOV binding of `buffer` as the AOV of `source`.
pxr::HdRenderPassAovBinding BindingOf(RenderVarSource source,
                                      RenderBuffer& buffer) {
  pxr::HdRenderPassAovBinding binding;
  binding.aovName = AovName(source);
  binding.renderBuffer = &buffer;
  binding.renderBufferId = buffer.GetId();
  return binding;
}

// The AOV bindings of `buffers`, those CreateBuffers gives `product`, by the
// names AovName gives their sources.
pxr::HdRenderPassAovBindingVector BindingsOf(const RenderProduct& product,
                                             const ProductBuffers& buffers) {
  pxr::HdRenderPassAovBindingVector bindings;
  bindings.reserve(buffers.size());
  for (const auto& [var, buffer] : std::views::zip(product.vars, buffers)) {
    bindings.push_back(BindingOf(var.source, *buffer));
  }
  for (const auto& [source, buffer] :
       std::views::zip(InternalInputs(product),
                       buffers | std::views::drop(product.vars.size()))) {
    bindings.push_back(BindingOf(source, *buffer));
  }
  return bindings;
}

}  // namespace

RenderTask::RenderTask(RenderDelegate& delegate, pxr::SdfPath settings,
                       DenoiseDevice denoise_device,
                       Display* UMBRA_NULLABLE display)
    : pxr::HdTask(TaskPath()),
      delegate_(&delegate),
      settings_(std::move(settings)),
      denoise_device_(std::move(denoise_device)),
      display_(display) {}

void RenderTask::SetOnCommitted(std::function<void()> on_committed) {
  on_committed_ = std::move(on_committed);
}

void RenderTask::Sync(pxr::HdSceneDelegate* UMBRA_NULLABLE /*delegate*/,
                      pxr::HdTaskContext* /*context*/,
                      pxr::HdDirtyBits* dirty_bits) {
  // The sprims (the render settings, the cameras, the lights) are synced
  // before the tasks, and the rprims after.
  render_tags_ = delegate_->RenderTags(settings_);
  delegate_->ChooseShutter(settings_);
  delegate_->ChooseDicing(settings_);
  delegate_->Place(render_tags_);
  *dirty_bits = pxr::HdChangeTracker::Clean;
}

void RenderTask::Prepare(pxr::HdTaskContext* /*context*/,
                         pxr::HdRenderIndex* index) {
  index_ = index;
}

void RenderTask::Execute(pxr::HdTaskContext* /*context*/) {
  // CommitResources ran between the syncs and the execution of the tasks.
  const Committed* const UMBRA_NULLABLE committed = delegate_->PeekCommitted();
  if (committed == nullptr) {
    result_ = std::nullopt;
    return;
  }
  if (on_committed_) {
    on_committed_();
  }
  result_ = Render(*committed);
}

const pxr::TfTokenVector& RenderTask::GetRenderTags() const {
  return render_tags_;
}

std::optional<std::expected<void, core::Error>> RenderTask::TakeResult() {
  return std::exchange(result_, std::nullopt);
}

std::expected<Denoiser * UMBRA_NULLABLE, core::Error> RenderTask::DenoiserOf(
    const RenderProduct& product) {
  if (!HasVar(product, RenderVarSource::kDenoisedColor)) {
    return nullptr;
  }
  if (denoiser_ == nullptr) {
    auto created = Denoiser::Create(denoise_device_);
    if (!created.has_value()) {
      return std::unexpected(std::move(created.error()));
    }
    denoiser_ = *std::move(created);
  }
  return denoiser_.get();
}

std::expected<void, core::Error> RenderTask::Render(
    const Committed& committed) {
  if (index_ == nullptr) {
    return std::unexpected(core::Error(core::ErrorCode::kInternal,
                                       "the render task was not prepared"));
  }
  const std::vector<RenderProduct>& products = committed.settings.products;
  buffers_.resize(products.size());
  while (passes_.size() < products.size()) {
    passes_.push_back(
        std::static_pointer_cast<RenderPass>(delegate_->CreateRenderPass(
            index_, pxr::HdRprimCollection(
                        pxr::HdTokens->geometry,
                        pxr::HdReprSelector(pxr::HdReprTokens->hull)))));
  }
  for (std::size_t index = 0; index < products.size(); ++index) {
    if (auto rendered = RenderOne(committed, index); !rendered.has_value()) {
      return rendered;
    }
  }
  return {};
}

std::expected<void, core::Error> RenderTask::RenderOne(
    const Committed& committed, std::size_t index) {
  const RenderProduct& product = committed.settings.products.at(index);
  auto buffers = CreateBuffers(product, index);
  if (!buffers.has_value()) {
    return std::unexpected(std::move(buffers.error()));
  }
  const auto denoiser = DenoiserOf(product);
  if (!denoiser.has_value()) {
    return std::unexpected(denoiser.error());
  }
  buffers_.at(index) = *std::move(buffers);
  const auto state = std::make_shared<pxr::HdRenderPassState>();
  state->SetAovBindings(BindingsOf(product, buffers_.at(index)));
  RenderPass& pass = *passes_.at(index);
  pass.SetProduct(product, committed.frame, *denoiser, display_);
  pass.Execute(state, render_tags_);
  return pass.TakeResult();
}

}  // namespace umbra::delegate::internal
