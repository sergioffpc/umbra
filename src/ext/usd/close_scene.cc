// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "close_scene.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range1f.h>
#include <pxr/base/gf/range2f.h>
#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/renderSettings.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/sceneIndexPrimView.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdRender/tokens.h>
#include <pxr/usd/usdRender/var.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hydra.h"
#include "material.h"
#include "mesh.h"
#include "motion.h"
#include "outputs.h"
#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/safety.h"
#include "umbra/scene/scene.h"

namespace umbra::ext::usd::internal {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;
using Product = pxr::HdRenderSettings::RenderProduct;
using Var = Product::RenderVar;

// The prims of one kind a warning reports: their count and the first.
class Tally {
 public:
  void Add(const pxr::SdfPath& path, std::string_view detail = {}) {
    if (count_ == 0) {
      first_ = path.GetString();
      detail_ = detail;
    }
    ++count_;
  }

  [[nodiscard]] std::size_t count() const { return count_; }
  // "/World/Car/Body" for one, "first /World/Car/Body" for more.
  [[nodiscard]] std::string First() const {
    return count_ == 1 ? first_ : "first " + first_;
  }
  [[nodiscard]] const std::string& detail() const UMBRA_LIFETIMEBOUND {
    return detail_;
  }

 private:
  std::size_t count_ = 0;
  std::string first_;
  std::string detail_;
};

// `singular` for one, `plural` otherwise.
std::string_view Plural(std::size_t count, std::string_view singular,
                        std::string_view plural) {
  return count == 1 ? singular : plural;
}

// The prims whose motion sampling changed, for the warnings that report
// them.
class MotionTallies {
 public:
  void Add(const pxr::SdfPath& path, MotionOutcome outcome) {
    switch (outcome) {
      case MotionOutcome::kSampled:
        break;
      case MotionOutcome::kResampledDown:
        resampled_.Add(path);
        break;
      case MotionOutcome::kTopologyChanges:
        changing_.Add(path);
        break;
    }
  }

  // "warning: usd: 3 prims change topology within the shutter, rendered
  // without motion blur (first /World/fluid)", and "warning: usd: 2 prims
  // have more than 16 motion samples within the shutter, resampled down to
  // 16 (first /World/sim)".
  void Report() const {
    if (changing_.count() > 0) {
      UMBRA_LOG_WARNING(
          kUsd,
          "{} {} topology within the shutter, rendered without motion blur "
          "({})",
          changing_.count(),
          Plural(changing_.count(), "prim changes", "prims change"),
          changing_.First());
    }
    if (resampled_.count() > 0) {
      UMBRA_LOG_WARNING(kUsd,
                        "{} {} more than {} motion samples within the "
                        "shutter, resampled down to {} ({})",
                        resampled_.count(),
                        Plural(resampled_.count(), "prim has", "prims have"),
                        kMaxMotionSamples, kMaxMotionSamples,
                        resampled_.First());
    }
  }

 private:
  Tally changing_;
  Tally resampled_;
};

// The cameras the settings and their products see through, converted once
// each, in meters, their transforms sampled over the shutter.
class Cameras {
 public:
  Cameras(const SyncedPrims& prims UMBRA_LIFETIMEBOUND, double meters_per_unit,
          MotionTallies& motion UMBRA_LIFETIMEBOUND)
      : prims_(&prims), meters_per_unit_(meters_per_unit), motion_(&motion) {}

  // The index of the camera at `path`, which `owner` names: fails when
  // there is none, or it is not a Camera.
  std::expected<std::size_t, Error> Index(const pxr::SdfPath& path,
                                          std::string_view owner);

  std::vector<scene::Camera> Take() { return std::move(cameras_); }

 private:
  [[nodiscard]] scene::Camera Convert(const CameraPrim& prim);
  // A length in scene units, in meters.
  [[nodiscard]] double Meters(float length) const {
    return static_cast<double>(length) * meters_per_unit_;
  }

  const SyncedPrims* prims_;
  double meters_per_unit_;
  MotionTallies* motion_;
  std::map<pxr::SdfPath, std::size_t> indices_;
  std::vector<scene::Camera> cameras_;
};

std::expected<std::size_t, Error> Cameras::Index(const pxr::SdfPath& path,
                                                 std::string_view owner) {
  if (path.IsEmpty()) {
    return std::unexpected(Error(ErrorCode::kInvalidArgument,
                                 std::format("{} have no camera", owner)));
  }
  if (const auto known = indices_.find(path); known != indices_.end()) {
    return known->second;
  }
  const auto synced = prims_->cameras.find(path);
  if (synced == prims_->cameras.end()) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("{}: the camera {} is not a Camera", owner,
                          path.GetString())));
  }
  cameras_.push_back(Convert(*synced->second));
  indices_.emplace(path, cameras_.size() - 1);
  return cameras_.size() - 1;
}

scene::Camera Cameras::Convert(const CameraPrim& prim) {
  const double scale = meters_per_unit_;
  // Lengths are in scene units, the apertures and focal length too once
  // Hydra has read them.
  scene::Camera camera;
  camera.path = prim.GetId().GetString();
  camera.projection = prim.GetProjection() == pxr::HdCamera::Orthographic
                          ? scene::Projection::kOrthographic
                          : scene::Projection::kPerspective;
  const SampledTransform camera_to_world = ResampleTransform(
      prim.transform_samples(), prim.GetTransform(), prims_->shutter);
  motion_->Add(prim.GetId(), camera_to_world.outcome);
  for (const pxr::GfMatrix4d& transform : camera_to_world.motion) {
    camera.camera_to_world.push_back(TransformInMeters(transform, scale));
  }
  camera.focal_length = Meters(prim.GetFocalLength());
  camera.aperture = {
      Meters(prim.GetHorizontalAperture()),
      Meters(prim.GetVerticalAperture()),
  };
  camera.aperture_offset = {
      Meters(prim.GetHorizontalApertureOffset()),
      Meters(prim.GetVerticalApertureOffset()),
  };
  const pxr::GfRange1f& clipping = prim.GetClippingRange();
  camera.clipping_range = {
      Meters(clipping.GetMin()),
      Meters(clipping.GetMax()),
  };
  camera.f_stop = static_cast<double>(prim.GetFStop());
  camera.focus_distance = Meters(prim.GetFocusDistance());
  return camera;
}

// The render vars left out, for the one warning that reports them.
class UnsupportedVars {
 public:
  void Add(std::string_view name, const pxr::SdfPath& product) {
    if (count_ == 0) {
      first_name_ = name;
      first_product_ = product.GetString();
    }
    ++count_;
  }

  // "warning: usd: 3 render vars not supported (lpe:C<RD>L, first in
  // /Render/beauty); left out".
  void Report() const {
    if (count_ == 1) {
      UMBRA_LOG_WARNING(kUsd,
                        "1 render var not supported ({}, in {}); left out",
                        first_name_, first_product_);
    } else if (count_ > 1) {
      UMBRA_LOG_WARNING(kUsd,
                        "{} render vars not supported ({}, first in {}); left "
                        "out",
                        count_, first_name_, first_product_);
    }
  }

 private:
  std::size_t count_ = 0;
  std::string first_name_;
  std::string first_product_;
};

// Whether a mesh has a displayColor primvar.
enum class DisplayColor : std::uint8_t {
  kAbsent,
  kPresent,
};

// The materials of the prototypes, each held once: those that translate,
// and umbra's two for the meshes whose material does not, or that have none.
class Materials {
 public:
  explicit Materials(const SyncedPrims& prims UMBRA_LIFETIMEBOUND)
      : prims_(&prims) {}

  // The index of the material bound to a mesh, or to one of its
  // GeomSubsets, at `material`: that material when it translates; else the
  // fallback material; for no material, the display color material when
  // the mesh has a displayColor, and the fallback material otherwise.
  std::size_t IndexOf(const pxr::SdfPath& material, DisplayColor display_color);

  // The error that stops the render: the first of the materials indexed.
  [[nodiscard]] const std::optional<Error>& fatal() const UMBRA_LIFETIMEBOUND {
    return fatal_;
  }

  // "warning: usd: 2 materials do not translate, rendered with the fallback
  // material (first /World/Looks/Skin: ...)", "warning: usd: 3 textures do
  // not resolve, rendered with their node's fallback value (first
  // /World/Looks/Car/Albedo: ./tex/a.exr)", and one per terminal umbra does
  // not take.
  void Report() const;

  std::vector<scene::MaterialNetwork> Take() { return std::move(networks_); }

 private:
  // The index of umbra's material in `slot`, which `make` makes once.
  std::size_t Own(std::optional<std::size_t>& slot,
                  scene::MaterialNetwork (*make)());

  const SyncedPrims* prims_;
  std::map<pxr::SdfPath, std::size_t> indices_;
  std::optional<std::size_t> fallback_;
  std::optional<std::size_t> display_color_;
  std::optional<Error> fatal_;
  std::vector<scene::MaterialNetwork> networks_;
};

std::size_t Materials::IndexOf(const pxr::SdfPath& material,
                               DisplayColor display_color) {
  const auto synced = prims_->materials.find(material);
  if (synced == prims_->materials.end()) {
    return display_color == DisplayColor::kPresent
               ? Own(display_color_, DisplayColorMaterial)
               : Own(fallback_, FallbackMaterial);
  }
  if (const auto known = indices_.find(material); known != indices_.end()) {
    return known->second;
  }
  const MaterialPrim& prim = *synced->second;
  if (!prim.translation().has_value() && !fatal_.has_value()) {
    fatal_ = prim.translation().error();
  }
  const scene::MaterialNetwork* network = prim.network();
  if (network == nullptr) {
    return Own(fallback_, FallbackMaterial);
  }
  networks_.push_back(*network);
  indices_.emplace(material, networks_.size() - 1);
  return networks_.size() - 1;
}

std::size_t Materials::Own(std::optional<std::size_t>& slot,
                           scene::MaterialNetwork (*make)()) {
  if (!slot.has_value()) {
    networks_.push_back(make());
    slot = networks_.size() - 1;
  }
  return *slot;
}

// The materials, the textures and the terminals the warnings about
// materials report.
class MaterialTallies {
 public:
  void Add(const pxr::SdfPath& path, const TranslatedMaterial& translation) {
    if (!translation.network.has_value()) {
      untranslated_.Add(path, translation.network.error());
    }
    for (const UnresolvedTexture& texture : translation.unresolved) {
      unresolved_.Add(texture.node, texture.asset);
    }
    for (const std::string& terminal : translation.unsupported_terminals) {
      terminals_[terminal].Add(path);
    }
  }

  void Report() const {
    ReportUntranslated();
    ReportUnresolved();
    ReportTerminals();
  }

 private:
  void ReportUntranslated() const;
  void ReportUnresolved() const;
  void ReportTerminals() const;

  Tally untranslated_;
  Tally unresolved_;
  std::map<std::string, Tally> terminals_;
};

void MaterialTallies::ReportUntranslated() const {
  if (untranslated_.count() > 0) {
    UMBRA_LOG_WARNING(
        kUsd,
        "{} {} not translate, rendered with the fallback material ({}: "
        "{})",
        untranslated_.count(),
        Plural(untranslated_.count(), "material does", "materials do"),
        untranslated_.First(), untranslated_.detail());
  }
}

void MaterialTallies::ReportUnresolved() const {
  if (unresolved_.count() > 0) {
    UMBRA_LOG_WARNING(
        kUsd,
        "{} {} not resolve, rendered with {} node's fallback value "
        "({}: {})",
        unresolved_.count(),
        Plural(unresolved_.count(), "texture does", "textures do"),
        Plural(unresolved_.count(), "its", "their"), unresolved_.First(),
        unresolved_.detail());
  }
}

void MaterialTallies::ReportTerminals() const {
  for (const auto& [terminal, tally] : terminals_) {
    UMBRA_LOG_WARNING(
        kUsd,
        "{} {} a {}, which umbra does not render yet; {} "
        "surface is rendered ({})",
        tally.count(),
        Plural(tally.count(), "material connects", "materials connect"),
        terminal, Plural(tally.count(), "its", "their"), tally.First());
  }
}

void Materials::Report() const {
  MaterialTallies tallies;
  for (const auto& [path, material] : prims_->materials) {
    if (material->translation().has_value()) {
      tallies.Add(path, *material->translation());
    }
  }
  tallies.Report();
}

// Whether `primvars` holds a displayColor.
DisplayColor DisplayColorOf(const std::vector<scene::Primvar>& primvars) {
  return std::ranges::any_of(primvars,
                             [](const scene::Primvar& primvar) {
                               return primvar.name == kDisplayColor;
                             })
             ? DisplayColor::kPresent
             : DisplayColor::kAbsent;
}

// Gives `faces`, those of a GeomSubset, the material `index` in
// `face_materials`, skipping those out of its range.
void MarkFaces(const std::vector<std::uint32_t>& faces, std::uint32_t index,
               std::vector<std::uint32_t>& face_materials) {
  for (const std::uint32_t face : faces) {
    if (face < face_materials.size()) {
      face_materials.at(face) = index;
    }
  }
}

// "1 mesh asks", "120 meshes ask".
std::string Meshes(std::size_t count, std::string_view singular_verb,
                   std::string_view plural_verb) {
  return count == 1 ? std::format("1 mesh {}", singular_verb)
                    : std::format("{} meshes {}", count, plural_verb);
}

// The meshes the Scene takes, as prototypes with their instances and their
// materials, and the tallies of those it does not, for their warnings.
class Geometry {
 public:
  Geometry(MotionTallies& motion UMBRA_LIFETIMEBOUND,
           Materials& materials UMBRA_LIFETIMEBOUND)
      : motion_(&motion), materials_(&materials) {}

  void Add(const pxr::SdfPath& path, MeshPrim& mesh);
  void Add(const UnsupportedPrim& prim);

  // "warning: usd: 120 meshes ask for catmullClark subdivision, rendered
  // as their cage (first /World/Car/Body)", and so on.
  void Report() const {
    ReportMeshes();
    ReportUnsupported();
  }

  std::vector<scene::Mesh> TakePrototypes() { return std::move(prototypes_); }
  std::vector<scene::Instance> TakeInstances() { return std::move(instances_); }

 private:
  MotionTallies* motion_;
  Materials* materials_;
  std::vector<scene::Mesh> prototypes_;
  std::vector<scene::Instance> instances_;
  std::map<std::string, Tally> subdivided_;
  Tally invalid_;
  std::map<pxr::TfToken, Tally> unsupported_;

  // Gives `prototype`, the mesh of `mesh`, its materials and those of its
  // triangles.
  void AssignMaterials(const MeshPrim& mesh, scene::Mesh& prototype);
  void ReportMeshes() const;
  void ReportUnsupported() const;
};

void Geometry::AssignMaterials(const MeshPrim& mesh, scene::Mesh& prototype) {
  const DisplayColor display_color = DisplayColorOf(prototype.primvars);
  prototype.materials.push_back(
      materials_->IndexOf(mesh.GetMaterialId(), display_color));
  const std::vector<MaterialSubset>& subsets = mesh.material_subsets();
  if (subsets.empty()) {
    return;
  }
  // The material of each source face, an index into prototype.materials.
  std::vector<std::uint32_t> face_materials(prototype.face_vertex_counts.size(),
                                            0);
  for (const MaterialSubset& subset : subsets) {
    const auto index = static_cast<std::uint32_t>(prototype.materials.size());
    prototype.materials.push_back(
        materials_->IndexOf(subset.material, display_color));
    MarkFaces(subset.faces, index, face_materials);
  }
  prototype.triangle_materials.reserve(prototype.source_faces.size());
  for (const std::uint32_t face : prototype.source_faces) {
    prototype.triangle_materials.push_back(face_materials.at(face));
  }
}

void Geometry::Add(const pxr::SdfPath& path, MeshPrim& mesh) {
  switch (mesh.state()) {
    case MeshState::kNotSynced:
    case MeshState::kInvisible:
      return;
    case MeshState::kInvalid:
      invalid_.Add(path, mesh.invalid_reason());
      return;
    case MeshState::kConverted:
      break;
  }
  std::vector<scene::Instance> instances = mesh.TakeInstances();
  if (instances.empty()) {
    return;
  }
  if (!mesh.subdivision_scheme().empty()) {
    subdivided_[mesh.subdivision_scheme()].Add(path);
  }
  motion_->Add(path, mesh.motion_outcome());
  scene::Mesh prototype = mesh.TakeMesh();
  AssignMaterials(mesh, prototype);
  for (scene::Instance& instance : instances) {
    instance.prototype = prototypes_.size();
    instances_.push_back(std::move(instance));
  }
  prototypes_.push_back(std::move(prototype));
}

void Geometry::Add(const UnsupportedPrim& prim) {
  if (prim.IsSynced()) {
    unsupported_[prim.type()].Add(prim.GetId());
  }
}

void Geometry::ReportMeshes() const {
  for (const auto& [scheme, tally] : subdivided_) {
    UMBRA_LOG_WARNING(kUsd, "{} for {} subdivision, rendered as {} cage ({})",
                      Meshes(tally.count(), "asks", "ask"), scheme,
                      Plural(tally.count(), "its", "their"), tally.First());
  }
  if (invalid_.count() > 0) {
    UMBRA_LOG_WARNING(kUsd, "{} invalid, dropped ({}: {})",
                      Meshes(invalid_.count(), "is", "are"), invalid_.First(),
                      invalid_.detail());
  }
}

void Geometry::ReportUnsupported() const {
  for (const auto& [type, tally] : unsupported_) {
    UMBRA_LOG_WARNING(kUsd, "{} {} {} not supported yet; left out ({})",
                      tally.count(), type.GetString(),
                      Plural(tally.count(), "prim", "prims"), tally.First());
  }
}

// Whether the light `prim` links geometry: whether hdsi gave its lightLink
// or its shadowLink a category, as it does to any collection but the
// default one of every prim.
bool IsLinked(const pxr::HdSceneIndexPrim& prim) {
  const pxr::HdContainerDataSourceHandle light =
      pxr::HdLightSchema::GetFromParent(prim.dataSource).GetContainer();
  if (light == nullptr) {
    return false;
  }
  const std::array<pxr::TfToken, 2> links = {
      pxr::HdTokens->lightLink,
      pxr::HdTokens->shadowLink,
  };
  return std::ranges::any_of(links, [&](const pxr::TfToken& link) {
    const auto category =
        pxr::HdTypedSampledDataSource<pxr::TfToken>::Cast(light->Get(link));
    return category != nullptr && !category->GetTypedValue(0.0F).IsEmpty();
  });
}

// "warning: usd: 3 lights link geometry, which umbra does not: every light
// lights everything (first /World/rim)"
// (docs/adr/0009-lights-light-everything.md).
void ReportLightLinking(const pxr::HdSceneIndexBaseRefPtr& scene_index) {
  Tally linked;
  for (const pxr::SdfPath& path : pxr::HdSceneIndexPrimView(
           scene_index, pxr::HdSceneIndexPrimView::Lexicographic)) {
    const pxr::HdSceneIndexPrim prim = scene_index->GetPrim(path);
    if (pxr::HdPrimTypeIsLight(prim.primType) && IsLinked(prim)) {
      linked.Add(path);
    }
  }
  if (linked.count() > 0) {
    UMBRA_LOG_WARNING(kUsd,
                      "{} {} geometry, which umbra does not: every light "
                      "lights everything ({})",
                      linked.count(),
                      Plural(linked.count(), "light links", "lights link"),
                      linked.First());
  }
}

// The quantity of a raw var named `name`, or none when umbra has no such
// var.
std::optional<scene::RenderVarSource> RawSource(std::string_view name) {
  static constexpr std::array<
      std::pair<std::string_view, scene::RenderVarSource>, 4>
      kSources = {
          {
              {"color", scene::RenderVarSource::kColor},
              {"depth", scene::RenderVarSource::kDepth},
              {"normal", scene::RenderVarSource::kNormal},
              {"albedo", scene::RenderVarSource::kAlbedo},
          },
  };
  for (const auto& [known, source] : kSources) {
    if (known == name) {
      return source;
    }
  }
  return std::nullopt;
}

// Whether `data_type` has half components: half, half3, color3h, ...
bool IsHalfType(std::string_view data_type) {
  return data_type.starts_with("half") || data_type.ends_with('h');
}

// The channel type of a var: half, unless its prim authors a dataType of
// more precision (float3, color3f, double), which gets float. The schema's
// fallback, color3f, is not authored: it gives half.
scene::ChannelType ChannelTypeOf(const Var& var, const pxr::UsdStage& stage) {
  const pxr::UsdRenderVar prim(stage.GetPrimAtPath(var.varPath));
  const bool authored = prim.GetDataTypeAttr().HasAuthoredValue();
  return authored && !IsHalfType(var.dataType.GetString())
             ? scene::ChannelType::kFloat
             : scene::ChannelType::kHalf;
}

// The vars of `product` umbra renders, the others added to `unsupported`.
std::vector<scene::RenderVar> ProductVars(const Product& product,
                                          const pxr::UsdStage& stage,
                                          UnsupportedVars& unsupported) {
  std::vector<scene::RenderVar> vars;
  for (const Var& var : product.renderVars) {
    // A var without a sourceName is the quantity it is named after.
    const std::string name = var.varPath.GetName();
    const std::string source_name =
        var.sourceName.empty() ? name : var.sourceName;
    const auto source = var.sourceType == pxr::UsdRenderTokens->raw
                            ? RawSource(source_name)
                            : std::nullopt;
    if (!source.has_value()) {
      unsupported.Add(source_name, product.productPath);
      continue;
    }
    vars.push_back({
        .name = name,
        .source = *source,
        .type = ChannelTypeOf(var, stage),
    });
  }
  return vars;
}

// The policy of a conformPolicy token: expandAperture, the fallback, for an
// unknown one.
scene::AspectRatioConformPolicy ConformPolicy(const pxr::TfToken& token) {
  using enum scene::AspectRatioConformPolicy;
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

// A product with the framing of `base` and no vars, for `file`.
scene::RenderProduct Framed(std::string file, std::size_t camera,
                            const SettingsBase& base) {
  return {
      .file = std::move(file),
      .camera = camera,
      .resolution = base.resolution,
      .pixel_aspect_ratio = base.pixel_aspect_ratio,
      .aspect_ratio_conform_policy =
          ConformPolicy(base.aspect_ratio_conform_policy),
      .data_window_ndc = base.data_window_ndc,
      .motion_blur = base.motion_blur,
      .depth_of_field = base.depth_of_field,
      .vars = {},
  };
}

// A data window as xmin, ymin, xmax, ymax.
std::array<float, 4> DataWindow(const pxr::GfRange2f& window) {
  const std::array<float, 2> min = Components<float, 2>(window.GetMin());
  const std::array<float, 2> max = Components<float, 2>(window.GetMax());
  return {min.at(0), min.at(1), max.at(0), max.at(1)};
}

// The framing of a flattened product, which inherits the settings'.
SettingsBase ProductBase(const Product& product) {
  return {
      .resolution = Components<std::int32_t, 2>(product.resolution),
      .pixel_aspect_ratio = product.pixelAspectRatio,
      .aspect_ratio_conform_policy = product.aspectRatioConformPolicy,
      .data_window_ndc = DataWindow(product.dataWindowNDC),
      .motion_blur = product.disableMotionBlur ? scene::MotionBlur::kDisabled
                                               : scene::MotionBlur::kEnabled,
      .depth_of_field = product.disableDepthOfField
                            ? scene::DepthOfField::kDisabled
                            : scene::DepthOfField::kEnabled,
  };
}

// Converts the products of the settings, as the render writes them.
class ProductConverter {
 public:
  ProductConverter(const CloseInputs& inputs UMBRA_LIFETIMEBOUND,
                   const pxr::UsdStage& stage UMBRA_LIFETIMEBOUND,
                   Cameras& cameras UMBRA_LIFETIMEBOUND)
      : inputs_(&inputs), stage_(&stage), cameras_(&cameras) {}

  std::expected<scene::RenderProduct, Error> Convert(const Product& product);

  // The product of settings without products: one color var.
  std::expected<scene::RenderProduct, Error> Implicit(std::size_t camera,
                                                      const SettingsBase& base);

  void Report() const { unsupported_.Report(); }

 private:
  const CloseInputs* inputs_;
  const pxr::UsdStage* stage_;
  Cameras* cameras_;
  UnsupportedVars unsupported_;
};

std::expected<scene::RenderProduct, Error> ProductConverter::Convert(
    const Product& product) {
  const std::string owner =
      std::format("render product {}", product.productPath.GetString());
  if (product.renderVars.empty()) {
    return std::unexpected(Error(ErrorCode::kInvalidArgument,
                                 std::format("{} has no orderedVars", owner)));
  }
  const auto camera = cameras_->Index(product.cameraPath, owner);
  if (!camera.has_value()) {
    return std::unexpected(camera.error());
  }
  std::string file = ExpandFrame(product.name.GetString(), inputs_->frame);
  if (auto prepared = PrepareOutput(file); !prepared.has_value()) {
    return std::unexpected(prepared.error());
  }
  scene::RenderProduct converted =
      Framed(std::move(file), *camera, ProductBase(product));
  converted.vars = ProductVars(product, *stage_, unsupported_);
  if (converted.vars.empty()) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("{} has no var umbra renders", owner)));
  }
  return converted;
}

std::expected<scene::RenderProduct, Error> ProductConverter::Implicit(
    std::size_t camera, const SettingsBase& base) {
  std::string file = ImplicitProductFile(inputs_->scene_path, inputs_->frame);
  if (auto prepared = PrepareOutput(file); !prepared.has_value()) {
    return std::unexpected(prepared.error());
  }
  scene::RenderProduct product = Framed(std::move(file), camera, base);
  product.vars.push_back({
      .name = "color",
      .source = scene::RenderVarSource::kColor,
      .type = scene::ChannelType::kHalf,
  });
  return product;
}

// The products of the settings, or their implicit one.
std::expected<std::vector<scene::RenderProduct>, Error> Products(
    const RenderSettingsPrim& settings, std::size_t camera,
    ProductConverter& converter) {
  std::vector<scene::RenderProduct> products;
  if (settings.GetRenderProducts().empty()) {
    auto implicit = converter.Implicit(camera, settings.base());
    if (!implicit.has_value()) {
      return std::unexpected(implicit.error());
    }
    products.push_back(std::move(*implicit));
    return products;
  }
  for (const Product& product : settings.GetRenderProducts()) {
    auto converted = converter.Convert(product);
    if (!converted.has_value()) {
      return std::unexpected(converted.error());
    }
    products.push_back(std::move(*converted));
  }
  return products;
}

// The meshes the Scene takes, with their instances and materials; the
// warnings about the geometry, its materials and its motion reported.
Geometry CloseGeometry(const SyncedPrims& prims, MotionTallies& motion,
                       Materials& materials) {
  Geometry geometry(motion, materials);
  for (const auto& [path, mesh] : prims.meshes) {
    geometry.Add(path, *mesh);
  }
  for (const auto& [path, prim] : prims.unsupported) {
    geometry.Add(*prim);
  }
  for (const auto& [path, instancer] : prims.instancers) {
    if (instancer->IsSynced()) {
      motion.Add(path, instancer->motion_outcome());
    }
  }
  geometry.Report();
  materials.Report();
  motion.Report();
  return geometry;
}

}  // namespace

std::expected<scene::Scene, Error> CloseScene(
    const CloseInputs& inputs, const pxr::UsdStage& stage,
    const pxr::HdSceneIndexBaseRefPtr& scene_index, const SyncedPrims& prims) {
  const auto found = prims.settings.find(inputs.settings);
  if (found == prims.settings.end()) {
    return std::unexpected(
        Error(ErrorCode::kNotFound, std::format("no RenderSettings prim at {}",
                                                inputs.settings.GetString())));
  }
  const RenderSettingsPrim& settings = *found->second;
  Materials materials(prims);
  MotionTallies motion;
  Cameras cameras(prims, inputs.meters_per_unit, motion);
  const auto camera = cameras.Index(
      settings.CameraPath(),
      std::format("render settings {}", inputs.settings.GetString()));
  if (!camera.has_value()) {
    return std::unexpected(camera.error());
  }
  ProductConverter converter(inputs, stage, cameras);
  auto products = Products(settings, *camera, converter);
  converter.Report();
  if (!products.has_value()) {
    return std::unexpected(products.error());
  }
  scene::RenderSettings converted{
      .path = inputs.settings.GetString(),
      .camera = *camera,
      .products = std::move(*products),
  };
  Geometry geometry = CloseGeometry(prims, motion, materials);
  if (materials.fatal().has_value()) {
    return std::unexpected(*materials.fatal());
  }
  ReportLightLinking(scene_index);
  return scene::Scene(std::move(converted), prims.shutter, cameras.Take(),
                      {
                          .materials = materials.Take(),
                          .prototypes = geometry.TakePrototypes(),
                          .instances = geometry.TakeInstances(),
                      });
}

}  // namespace umbra::ext::usd::internal
