// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "mesh_prim.h"

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/dataSourceLocator.h>
#include <pxr/imaging/hd/dirtyBitsTranslator.h>
#include <pxr/imaging/hd/enums.h>
#include <pxr/imaging/hd/extComputationUtils.h>
#include <pxr/imaging/hd/geomSubset.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/usd/sdf/path.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "dicing.h"
#include "host_copy.h"
#include "instancer.h"
#include "light.h"
#include "material.h"
#include "mesh.h"
#include "motion.h"
#include "motion_samples.h"
#include "sync_counter.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"

namespace umbra::delegate::internal {
namespace {

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

// The interpolations of the primvars a mesh takes.
constexpr std::array kMeshInterpolations = {
    pxr::HdInterpolationConstant,    pxr::HdInterpolationUniform,
    pxr::HdInterpolationVarying,     pxr::HdInterpolationVertex,
    pxr::HdInterpolationFaceVarying,
};

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
  return FromArray(found->second);
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

}  // namespace

void MapEmissionToDirtyBit() {
  static std::once_flag once;
  std::call_once(once, [] {
    pxr::HdDirtyBitsTranslator::RegisterTranslatorsForCustomRprimType(
        pxr::HdPrimTypeTokens->mesh, EmissionBitsOf, EmissionLocatorsOf);
  });
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
  if (state_ != MeshState::kConverted || changes_ != host::kWholeMesh) {
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
  changes_ = host::kWholeMesh;
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
      .primvars = ReadPrimvars(delegate, GetId(), kMeshInterpolations,
                               PrimvarIndices::kKept),
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
  const Placement placement =
      PlaceInstances(delegate, GetId(), GetInstancerId(), shutter);
  placement_outcome_ = placement.outcome;
  instances_ =
      HostInstances(placement.instances, meters_per_unit_, KeepPrimvars::kYes);
  instance_count_ = instances_.size();
}

void MeshPrim::ReadEmission(pxr::HdSceneDelegate& delegate) {
  emission_notes_.clear();
  mesh_.emission = ReadMeshLight(delegate, GetId(), emission_notes_);
}

void MeshPrim::KeepChanged() {
  using device::HasAny;
  const host::MeshMemberFlags& members = host::kMeshMembers;
  if (!HasAny(changes_, members.points)) {
    mesh_.points = {};
  }
  if (!HasAny(changes_, members.topology)) {
    mesh_.triangles = {};
    mesh_.source_faces = {};
    mesh_.face_vertex_counts = {};
  }
  if (!HasAny(changes_, members.primvars)) {
    mesh_.normals = {};
    mesh_.primvars = {};
  }
  if (!HasAny(changes_, members.materials)) {
    mesh_.triangle_materials = {};
  }
  if (!HasAny(changes_, members.instances)) {
    instances_ = {};
  }
  if (!HasAny(changes_, members.emission)) {
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

}  // namespace umbra::delegate::internal
