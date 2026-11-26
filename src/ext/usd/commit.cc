// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "commit.h"

#include <pxr/base/gf/range2f.h>
#include <pxr/base/tf/token.h>
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
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "host_copy.h"
#include "hydra.h"
#include "light.h"
#include "material.h"
#include "motion.h"
#include "outputs.h"
#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/number_format.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/device/render_data.h"
#include "umbra/ext/usd/render_settings.h"
#include "views.h"

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

// The cameras the settings and their products see through, each added
// once.
class Cameras {
 public:
  Cameras(const SyncedPrims& prims UMBRA_LIFETIMEBOUND,
          MotionTallies& motion UMBRA_LIFETIMEBOUND,
          ChangeSetBuilder& changes UMBRA_LIFETIMEBOUND)
      : prims_(&prims), motion_(&motion), changes_(&changes) {}

  // The id of the camera at `path`, which `owner` names: fails when there
  // is none, or it is not a Camera.
  std::expected<device::CameraId, Error> Index(const pxr::SdfPath& path,
                                               std::string_view owner);

 private:
  const SyncedPrims* prims_;
  MotionTallies* motion_;
  ChangeSetBuilder* changes_;
  std::map<pxr::SdfPath, device::CameraId> ids_;
};

std::expected<device::CameraId, Error> Cameras::Index(const pxr::SdfPath& path,
                                                      std::string_view owner) {
  if (path.IsEmpty()) {
    return std::unexpected(Error(ErrorCode::kInvalidArgument,
                                 std::format("{} have no camera", owner)));
  }
  if (const auto known = ids_.find(path); known != ids_.end()) {
    return known->second;
  }
  const auto synced = prims_->cameras.find(path);
  if (synced == prims_->cameras.end()) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("{}: the camera {} is not a Camera", owner,
                          path.GetString())));
  }
  const CameraPrim& camera = *synced->second;
  motion_->Add(path, camera.motion_outcome());
  const device::CameraId camera_id = changes_->AddCamera(camera.camera());
  ids_.emplace(path, camera_id);
  return camera_id;
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

// The materials of the prototypes, each added once: those that translate,
// and umbra's two for the meshes whose material does not, or that have none.
class Materials {
 public:
  Materials(const SyncedPrims& prims UMBRA_LIFETIMEBOUND,
            ChangeSetBuilder& changes UMBRA_LIFETIMEBOUND)
      : prims_(&prims), changes_(&changes) {}

  // The id of the material bound to a mesh, or to one of its GeomSubsets,
  // at `material`: that material when it translates; else the fallback
  // material; for no material, the display color material when the mesh
  // has a displayColor, and the fallback material otherwise.
  device::MaterialId IdOf(const pxr::SdfPath& material,
                          DisplayColor display_color);

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

 private:
  // A material of umbra's, made once, and its id.
  struct Own {
    host::MaterialNetwork network;
    device::MaterialId id{};
  };

  // The id of umbra's material in `slot`, which `make` makes once.
  device::MaterialId IdOf(std::optional<Own>& slot,
                          host::MaterialNetwork (*make)());

  const SyncedPrims* prims_;
  ChangeSetBuilder* changes_;
  std::map<pxr::SdfPath, device::MaterialId> ids_;
  std::optional<Own> fallback_;
  std::optional<Own> display_color_;
  std::optional<Error> fatal_;
};

device::MaterialId Materials::IdOf(const pxr::SdfPath& material,
                                   DisplayColor display_color) {
  const auto synced = prims_->materials.find(material);
  if (synced == prims_->materials.end()) {
    return display_color == DisplayColor::kPresent
               ? IdOf(display_color_, DisplayColorMaterial)
               : IdOf(fallback_, FallbackMaterial);
  }
  if (const auto known = ids_.find(material); known != ids_.end()) {
    return known->second;
  }
  const MaterialPrim& prim = *synced->second;
  if (!prim.translation().has_value() && !fatal_.has_value()) {
    fatal_ = prim.translation().error();
  }
  const host::MaterialNetwork* network = prim.network();
  if (network == nullptr) {
    return IdOf(fallback_, FallbackMaterial);
  }
  const device::MaterialId material_id = changes_->AddMaterial(*network);
  ids_.emplace(material, material_id);
  return material_id;
}

device::MaterialId Materials::IdOf(std::optional<Own>& slot,
                                   host::MaterialNetwork (*make)()) {
  if (!slot.has_value()) {
    // The network is in place before the builder views it.
    slot.emplace(make(), device::MaterialId{});
    slot->id = changes_->AddMaterial(slot->network);
  }
  return slot->id;
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
DisplayColor DisplayColorOf(const std::vector<host::Primvar>& primvars) {
  return std::ranges::any_of(primvars,
                             [](const host::Primvar& primvar) {
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

// The meshes committed, as prototypes with their instances and their
// materials, and the tallies of those that are not, for their warnings.
class Geometry {
 public:
  Geometry(MotionTallies& motion UMBRA_LIFETIMEBOUND,
           Materials& materials UMBRA_LIFETIMEBOUND,
           ChangeSetBuilder& changes UMBRA_LIFETIMEBOUND)
      : motion_(&motion), materials_(&materials), changes_(&changes) {}

  void Add(const pxr::SdfPath& path, MeshPrim& mesh);
  void Add(const UnsupportedPrim& prim);

  // "warning: usd: 120 meshes ask for catmullClark subdivision, rendered
  // as their cage (first /World/Car/Body)", and so on.
  void Report() const {
    ReportMeshes();
    ReportUnsupported();
  }

 private:
  MotionTallies* motion_;
  Materials* materials_;
  ChangeSetBuilder* changes_;
  std::map<std::string, Tally> subdivided_;
  Tally invalid_;
  std::map<pxr::TfToken, Tally> unsupported_;
  Tally mesh_lights_;

  // Gives `prototype`, the host copy of `mesh`, its materials and those of
  // its triangles.
  void AssignMaterials(const MeshPrim& mesh, host::Mesh& prototype);
  void ReportMeshes() const;
  void ReportUnsupported() const;
};

void Geometry::AssignMaterials(const MeshPrim& mesh, host::Mesh& prototype) {
  const DisplayColor display_color = DisplayColorOf(prototype.primvars);
  prototype.materials.push_back(
      materials_->IdOf(mesh.GetMaterialId(), display_color));
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
        materials_->IdOf(subset.material, display_color));
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
  if (mesh.instances().empty()) {
    return;
  }
  if (!mesh.subdivision_scheme().empty()) {
    subdivided_[mesh.subdivision_scheme()].Add(path);
  }
  if (mesh.IsMeshLight()) {
    mesh_lights_.Add(path);
  }
  motion_->Add(path, mesh.motion_outcome());
  AssignMaterials(mesh, mesh.mesh());
  changes_->AddPrototype(mesh.mesh(), mesh.instances());
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
  if (mesh_lights_.count() > 0) {
    UMBRA_LOG_WARNING(
        kUsd, "{} not supported yet; {} rendered without emission ({})",
        Plural(mesh_lights_.count(), "1 mesh light",
               std::format("{} mesh lights", mesh_lights_.count())),
        Plural(mesh_lights_.count(), "its mesh", "their meshes"),
        mesh_lights_.First());
  }
}

// The words of the warning about one LightIssue: "{count} {subject}
// {predicate} ({first}{: detail})", each for one light and for more.
struct IssueWords {
  LightIssue issue;
  std::array<std::string_view, 2> subject;
  std::array<std::string_view, 2> predicate;
};

constexpr std::array<IssueWords, 9> kIssueWords = {
    IssueWords{
        .issue = LightIssue::kFilters,
        .subject = {"light has", "lights have"},
        .predicate =
            {
                "light filters, which umbra does not take yet; ignored",
                "light filters, which umbra does not take yet; ignored",
            },
    },
    IssueWords{
        .issue = LightIssue::kIesProfile,
        .subject = {"light is", "lights are"},
        .predicate =
            {
                "shaped by an IES profile, which umbra does not take yet; "
                "ignored",
                "shaped by an IES profile, which umbra does not take yet; "
                "ignored",
            },
    },
    IssueWords{
        .issue = LightIssue::kNonPositiveTemperature,
        .subject = {"light enables", "lights enable"},
        .predicate =
            {
                "a color temperature not above 0 K; its color is used alone",
                "a color temperature not above 0 K; their color is used alone",
            },
    },
    IssueWords{
        .issue = LightIssue::kDiffuseSpecular,
        .subject = {"light scales", "lights scale"},
        .predicate =
            {
                "its diffuse or specular contribution, which umbra does not: "
                "lights are physical",
                "their diffuse or specular contribution, which umbra does not: "
                "lights are physical",
            },
    },
    IssueWords{
        .issue = LightIssue::kFocus,
        .subject = {"light focuses", "lights focus"},
        .predicate =
            {
                "its emission (shaping:focus), which umbra does not: lights "
                "are physical",
                "their emission (shaping:focus), which umbra does not: lights "
                "are physical",
            },
    },
    IssueWords{
        .issue = LightIssue::kConeSoftness,
        .subject = {"light softens", "lights soften"},
        .predicate =
            {
                "the edge of its cone, which umbra does not: lights are "
                "physical",
                "the edge of their cone, which umbra does not: lights are "
                "physical",
            },
    },
    IssueWords{
        .issue = LightIssue::kShadowControls,
        .subject = {"light authors", "lights author"},
        .predicate =
            {
                "shadow controls (ShadowAPI), which umbra does not take: "
                "lights are physical",
                "shadow controls (ShadowAPI), which umbra does not take: "
                "lights are physical",
            },
    },
    IssueWords{
        .issue = LightIssue::kColorNotConverted,
        .subject = {"light's color does", "lights' colors do"},
        .predicate =
            {
                "not convert into ACEScg, rendered white",
                "not convert into ACEScg, rendered white",
            },
    },
    IssueWords{
        .issue = LightIssue::kTextureNotRead,
        .subject = {"light texture is", "light textures are"},
        .predicate =
            {
                "not read, rendered with its light's color",
                "not read, rendered with their light's color",
            },
    },
};

// "warning: usd: 2 lights have light filters, which umbra does not take
// yet; ignored (first /World/key)", and so on.
void ReportLightIssue(LightIssue issue, const Tally& tally) {
  const auto* words = std::ranges::find(kIssueWords, issue, &IssueWords::issue);
  if (words == kIssueWords.end()) {
    return;
  }
  const std::size_t form = tally.count() == 1 ? 0 : 1;
  UMBRA_LOG_WARNING(kUsd, "{} {} {} ({}{}{})", tally.count(),
                    words->subject.at(form), words->predicate.at(form),
                    tally.First(), tally.detail().empty() ? "" : ": ",
                    tally.detail());
}

// The lights committed, those converted with an instance or more, and the
// tallies of those that are not and of what they author that umbra does
// not take, for their warnings.
class Lights {
 public:
  Lights(MotionTallies& motion UMBRA_LIFETIMEBOUND,
         ChangeSetBuilder& changes UMBRA_LIFETIMEBOUND)
      : motion_(&motion), changes_(&changes) {}

  void Add(const pxr::SdfPath& path, const LightPrim& light);

  // "warning: usd: 2 lights have light filters, which umbra does not take
  // yet; ignored (first /World/key)", and so on.
  void Report() const;

  // The error that stops the render: the first of the lights added.
  [[nodiscard]] const std::optional<Error>& fatal() const UMBRA_LIFETIMEBOUND {
    return fatal_;
  }

 private:
  MotionTallies* motion_;
  ChangeSetBuilder* changes_;
  std::map<LightIssue, Tally> issues_;
  std::map<std::string, Tally> unsupported_;
  std::optional<Error> fatal_;
};

void Lights::Add(const pxr::SdfPath& path, const LightPrim& light) {
  switch (light.state()) {
    case LightState::kHidden:
      return;
    case LightState::kUnsupported:
      unsupported_[light.unsupported_type()].Add(path);
      return;
    case LightState::kConverted:
      break;
  }
  if (light.fatal().has_value() && !fatal_.has_value()) {
    fatal_ = light.fatal();
  }
  for (const LightNote& note : light.notes()) {
    issues_[note.issue].Add(path, note.detail);
  }
  motion_->Add(path, light.motion_outcome());
  if (!light.light().instances.empty()) {
    changes_->AddLight(light.light());
  }
}

void Lights::Report() const {
  for (const auto& [type, tally] : unsupported_) {
    UMBRA_LOG_WARNING(kUsd, "{} {} {} not supported yet; left out ({})",
                      tally.count(), type,
                      Plural(tally.count(), "prim", "prims"), tally.First());
  }
  for (const auto& [issue, tally] : issues_) {
    ReportLightIssue(issue, tally);
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
std::optional<RenderVarSource> RawSource(std::string_view name) {
  static constexpr std::array<std::pair<std::string_view, RenderVarSource>, 4>
      kSources = {
          {
              {"color", RenderVarSource::kColor},
              {"depth", RenderVarSource::kDepth},
              {"normal", RenderVarSource::kNormal},
              {"albedo", RenderVarSource::kAlbedo},
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
ChannelType ChannelTypeOf(const Var& var, const pxr::UsdStage& stage) {
  const pxr::UsdRenderVar prim(stage.GetPrimAtPath(var.varPath));
  const bool authored = prim.GetDataTypeAttr().HasAuthoredValue();
  return authored && !IsHalfType(var.dataType.GetString()) ? ChannelType::kFloat
                                                           : ChannelType::kHalf;
}

// The vars of `product` umbra renders, the others added to `unsupported`.
std::vector<RenderVar> ProductVars(const Product& product,
                                   const pxr::UsdStage& stage,
                                   UnsupportedVars& unsupported) {
  std::vector<RenderVar> vars;
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

// A product with the framing of `base` and no vars, for `file`.
RenderProduct Framed(std::string file, device::CameraId camera,
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
      .motion_blur = product.disableMotionBlur ? MotionBlur::kDisabled
                                               : MotionBlur::kEnabled,
      .depth_of_field = product.disableDepthOfField ? DepthOfField::kDisabled
                                                    : DepthOfField::kEnabled,
  };
}

// Converts the products of the settings, as the render writes them.
class ProductConverter {
 public:
  ProductConverter(const CommitInputs& inputs UMBRA_LIFETIMEBOUND,
                   const pxr::UsdStage& stage UMBRA_LIFETIMEBOUND,
                   Cameras& cameras UMBRA_LIFETIMEBOUND)
      : inputs_(&inputs), stage_(&stage), cameras_(&cameras) {}

  std::expected<RenderProduct, Error> Convert(const Product& product);

  // The product of settings without products: one color var.
  std::expected<RenderProduct, Error> Implicit(device::CameraId camera,
                                               const SettingsBase& base);

  void Report() const { unsupported_.Report(); }

 private:
  const CommitInputs* inputs_;
  const pxr::UsdStage* stage_;
  Cameras* cameras_;
  UnsupportedVars unsupported_;
};

std::expected<RenderProduct, Error> ProductConverter::Convert(
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
  RenderProduct converted =
      Framed(std::move(file), *camera, ProductBase(product));
  converted.vars = ProductVars(product, *stage_, unsupported_);
  if (converted.vars.empty()) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("{} has no var umbra renders", owner)));
  }
  return converted;
}

std::expected<RenderProduct, Error> ProductConverter::Implicit(
    device::CameraId camera, const SettingsBase& base) {
  std::string file = ImplicitProductFile(inputs_->scene_path, inputs_->frame);
  if (auto prepared = PrepareOutput(file); !prepared.has_value()) {
    return std::unexpected(prepared.error());
  }
  RenderProduct product = Framed(std::move(file), camera, base);
  product.vars.push_back({
      .name = "color",
      .source = RenderVarSource::kColor,
      .type = ChannelType::kHalf,
  });
  return product;
}

// The products of the settings, or their implicit one.
std::expected<std::vector<RenderProduct>, Error> Products(
    const RenderSettingsPrim& settings, device::CameraId camera,
    ProductConverter& converter) {
  std::vector<RenderProduct> products;
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

// The lights committed; the warnings about them reported.
Lights CommitLights(const SyncedPrims& prims, MotionTallies& motion,
                    ChangeSetBuilder& changes) {
  Lights lights(motion, changes);
  for (const auto& [path, light] : prims.lights) {
    lights.Add(path, *light);
  }
  lights.Report();
  return lights;
}

// Commits the meshes, with their instances and materials; the warnings
// about the geometry, its materials and its motion reported.
void CommitGeometry(const SyncedPrims& prims, MotionTallies& motion,
                    Materials& materials, ChangeSetBuilder& changes) {
  Geometry geometry(motion, materials, changes);
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
}

// Adds what the frame renders to `changes`, its geometry with its materials
// and its lights, the warnings about them reported; or gives the error that
// stops the render.
std::expected<void, Error> CommitContent(
    const SyncedPrims& prims, const pxr::HdSceneIndexBaseRefPtr& scene_index,
    MotionTallies& motion, Materials& materials, ChangeSetBuilder& changes) {
  const Lights lights = CommitLights(prims, motion, changes);
  CommitGeometry(prims, motion, materials, changes);
  if (auto fatal = materials.fatal().or_else([&] { return lights.fatal(); });
      fatal.has_value()) {
    return std::unexpected(std::move(*fatal));
  }
  ReportLightLinking(scene_index);
  return {};
}

// "render/beauty.0012.exr [color, depth]".
std::string DescribeProduct(const RenderProduct& product) {
  std::string vars;
  for (const RenderVar& var : product.vars) {
    vars += vars.empty() ? var.name : ", " + var.name;
  }
  return std::format("{} [{}]", product.file, vars);
}

// "1.24M triangles in 3.4k meshes, 18 primvars, 2.1M instances of 3.4k
// prototypes": the primvars counted by name, the normals apart.
std::string DescribeGeometry(const device::ChangeSet& changes) {
  std::size_t triangles = 0;
  std::size_t instances = 0;
  std::set<std::string_view> primvars;
  for (const device::Prototype& prototype : changes.prototypes.added) {
    triangles += prototype.triangles.size();
    instances += prototype.instances.size();
    for (const device::Primvar& primvar : prototype.primvars) {
      primvars.insert(primvar.name);
    }
  }
  const std::size_t meshes = changes.prototypes.added.size();
  return std::format(
      "{} triangle{} in {} mesh{}, {} primvar{}, {} instance{} of {} "
      "prototype{}",
      core::FormatCount(triangles), triangles == 1 ? "" : "s",
      core::FormatCount(meshes), meshes == 1 ? "" : "es",
      core::FormatCount(primvars.size()), primvars.size() == 1 ? "" : "s",
      core::FormatCount(instances), instances == 1 ? "" : "s",
      core::FormatCount(meshes), meshes == 1 ? "" : "s");
}

// The kinds of material the debug line counts.
struct MaterialKinds {
  std::size_t materialx = 0;
  std::size_t preview_surface = 0;
  std::size_t osl = 0;
};

// Counts `material` into `kinds`: OSL when a node is an .oso shader,
// UsdPreviewSurface when its surface is, MaterialX otherwise.
void CountKind(const device::MaterialNetwork& material, MaterialKinds& kinds) {
  if (std::ranges::any_of(material.nodes, [](const device::MaterialNode& node) {
        return node.kind == device::ShaderKind::kOsl;
      })) {
    ++kinds.osl;
  } else if (material.nodes.subspan(material.surface.node).front().shader ==
             "ND_UsdPreviewSurface_surfaceshader") {
    ++kinds.preview_surface;
  } else {
    ++kinds.materialx;
  }
}

// "MaterialX 40, UsdPreviewSurface 15, OSL 2": the kinds counted, but those
// of none.
std::string DescribeKinds(const MaterialKinds& kinds) {
  const auto named = std::to_array<std::pair<std::string_view, std::size_t>>({
      {"MaterialX", kinds.materialx},
      {"UsdPreviewSurface", kinds.preview_surface},
      {"OSL", kinds.osl},
  });
  std::string counts;
  for (const auto& [name, count] : named) {
    if (count > 0) {
      counts += std::format("{}{} {}", counts.empty() ? "" : ", ", name,
                            core::FormatCount(count));
    }
  }
  return counts;
}

// The prototypes whose own material is the display color material.
std::size_t CountOnDisplayColor(const device::ChangeSet& changes) {
  const std::span<const device::MaterialNetwork> materials =
      changes.materials.added;
  return static_cast<std::size_t>(std::ranges::count_if(
      changes.prototypes.added, [&](const device::Prototype& prototype) {
        return !prototype.materials.empty() &&
               materials
                       .subspan(static_cast<std::size_t>(
                           prototype.materials.front()))
                       .front()
                       .path == kDisplayColorMaterial;
      }));
}

// Inserts into `files` those of the `textures` that `node` reads.
void InsertTextures(const device::MaterialNode& node,
                    std::span<const device::Texture> textures,
                    std::set<std::string_view>& files) {
  for (const device::Parameter& parameter : node.parameters) {
    if (const auto* texture =
            std::get_if<device::TextureId>(&parameter.value)) {
      files.insert(
          textures.subspan(static_cast<std::size_t>(*texture)).front().file);
    }
  }
}

// The files of the textures the materials of `changes` read.
std::set<std::string_view> MaterialTextures(const device::ChangeSet& changes) {
  std::set<std::string_view> files;
  for (const device::MaterialNetwork& material : changes.materials.added) {
    for (const device::MaterialNode& node : material.nodes) {
      InsertTextures(node, changes.textures.added, files);
    }
  }
  return files;
}

// "57 materials (MaterialX 40, UsdPreviewSurface 15, OSL 2), 210 textures,
// 300 meshes on displayColor": the textures counted by file.
std::string DescribeMaterials(const device::ChangeSet& changes) {
  MaterialKinds kinds;
  for (const device::MaterialNetwork& material : changes.materials.added) {
    CountKind(material, kinds);
  }
  const std::string counts = DescribeKinds(kinds);
  const std::size_t textures = MaterialTextures(changes).size();
  const std::size_t materials = changes.materials.added.size();
  const std::size_t on_display_color = CountOnDisplayColor(changes);
  return std::format("{} material{}{}, {} texture{}, {} mesh{} on displayColor",
                     core::FormatCount(materials), materials == 1 ? "" : "s",
                     counts.empty() ? "" : std::format(" ({})", counts),
                     core::FormatCount(textures), textures == 1 ? "" : "s",
                     core::FormatCount(on_display_color),
                     on_display_color == 1 ? "" : "es");
}

// "14 lights (rect 8, sphere 4, distant 1, dome 1)": the lights counted by
// kind, one for each instance, the most first.
std::string DescribeLights(const device::ChangeSet& changes) {
  constexpr std::array<std::string_view, 6> kKinds = device::kLightShapeNames;
  std::array<std::size_t, kKinds.size()> counts{};
  std::size_t lights = 0;
  for (const device::Light& light : changes.lights.added) {
    counts.at(light.shape.index()) += light.instances.size();
    lights += light.instances.size();
  }
  std::array<std::size_t, kKinds.size()> order{};
  std::ranges::iota(order, std::size_t{0});
  std::ranges::stable_sort(
      order, std::ranges::greater{},
      [&counts](std::size_t kind) { return counts.at(kind); });
  std::string kinds;
  for (const std::size_t kind : order) {
    if (counts.at(kind) > 0) {
      kinds += std::format("{}{} {}", kinds.empty() ? "" : ", ",
                           kKinds.at(kind), core::FormatCount(counts.at(kind)));
    }
  }
  return std::format("{} light{}{}", core::FormatCount(lights),
                     lights == 1 ? "" : "s",
                     kinds.empty() ? "" : std::format(" ({})", kinds));
}

// The motion of what the debug line counts: how many move, and the most
// motion samples of any.
struct MotionCount {
  std::size_t moving = 0;
  std::size_t samples = 1;

  void Add(std::size_t count) {
    moving += count > 1 ? 1 : 0;
    samples = std::max(samples, count);
  }
};

// "shutter -0.25..0.25, 1.2k moving (max 4 samples)": the prototypes whose
// points move and the instances that move, of prototypes and of lights, and
// the most motion samples of any of them or of a camera.
std::string DescribeMotion(const device::ChangeSet& changes,
                           const device::Shutter& shutter) {
  MotionCount motion;
  for (const device::Prototype& prototype : changes.prototypes.added) {
    motion.Add(prototype.points.size());
    for (const device::Instance& instance : prototype.instances) {
      motion.Add(instance.object_to_world.size());
    }
  }
  for (const device::Light& light : changes.lights.added) {
    for (const device::Instance& instance : light.instances) {
      motion.Add(instance.object_to_world.size());
    }
  }
  for (const device::Camera& camera : changes.cameras.added) {
    motion.samples = std::max(motion.samples, camera.camera_to_world.size());
  }
  return std::format("shutter {}..{}, {} moving (max {} sample{})",
                     shutter.open, shutter.close,
                     core::FormatCount(motion.moving), motion.samples,
                     motion.samples == 1 ? "" : "s");
}

// "settings /Render/main, camera /World/cam, 1 product: render/beauty.0012.exr
// [color, depth], 1.24M triangles in 3.4k meshes, 18 primvars, 2.1M
// instances of 3.4k prototypes, 57 materials (MaterialX 40,
// UsdPreviewSurface 15, OSL 2), 210 textures, 300 meshes on displayColor,
// 14 lights (rect 8, sphere 4, distant 1, dome 1), shutter -0.25..0.25, 1.2k
// moving (max 4 samples)".
std::string Describe(const RenderSettings& settings,
                     const device::ChangeSet& changes,
                     const device::Shutter& shutter) {
  std::string products;
  for (const RenderProduct& product : settings.products) {
    products += (products.empty() ? "" : ", ") + DescribeProduct(product);
  }
  const std::size_t count = settings.products.size();
  const std::string_view camera =
      changes.cameras.added.subspan(static_cast<std::size_t>(settings.camera))
          .front()
          .path;
  return std::format("settings {}, camera {}, {} product{}: {}, {}, {}, {}, {}",
                     settings.path, camera, count, count == 1 ? "" : "s",
                     products, DescribeGeometry(changes),
                     DescribeMaterials(changes), DescribeLights(changes),
                     DescribeMotion(changes, shutter));
}

// Commits `changes` to each of `devices`, in order: fails with the error of
// the first that fails, naming it.
std::expected<void, Error> CommitToDevices(
    const device::ChangeSet& changes,
    std::span<device::Device* const> devices) {
  for (device::Device* const device : devices) {
    if (auto committed = device->Commit(changes); !committed.has_value()) {
      return std::unexpected(Error(committed.error().code(),
                                   std::format("device {}: {}", device->name(),
                                               committed.error().message())));
    }
  }
  return {};
}

// The render settings of `settings`, at the path `inputs` name, seen
// through `camera`, with their products; `cameras` indexes those of the
// products.
std::expected<RenderSettings, Error> SettingsOf(
    const CommitInputs& inputs, const pxr::UsdStage& stage,
    const RenderSettingsPrim& settings, device::CameraId camera,
    Cameras& cameras) {
  ProductConverter converter(inputs, stage, cameras);
  auto products = Products(settings, camera, converter);
  converter.Report();
  if (!products.has_value()) {
    return std::unexpected(products.error());
  }
  return RenderSettings{
      .path = inputs.settings.GetString(),
      .camera = camera,
      .products = std::move(*products),
  };
}

}  // namespace

std::expected<Committed, Error> Commit(
    const CommitInputs& inputs, const pxr::UsdStage& stage,
    const pxr::HdSceneIndexBaseRefPtr& scene_index, SyncedPrims& prims,
    std::span<device::Device* const> devices) {
  const auto found = prims.settings.find(inputs.settings);
  if (found == prims.settings.end()) {
    return std::unexpected(
        Error(ErrorCode::kNotFound, std::format("no RenderSettings prim at {}",
                                                inputs.settings.GetString())));
  }
  const RenderSettingsPrim& settings = *found->second;
  ChangeSetBuilder changes;
  Materials materials(prims, changes);
  MotionTallies motion;
  Cameras cameras(prims, motion, changes);
  const auto camera = cameras.Index(
      settings.CameraPath(),
      std::format("render settings {}", inputs.settings.GetString()));
  if (!camera.has_value()) {
    return std::unexpected(camera.error());
  }
  auto render_settings = SettingsOf(inputs, stage, settings, *camera, cameras);
  if (!render_settings.has_value()) {
    return std::unexpected(std::move(render_settings.error()));
  }
  Committed committed{
      .settings = std::move(*render_settings),
      .frame = {.shutter = prims.shutter, .camera = *camera},
      .description = {},
  };
  if (auto content =
          CommitContent(prims, scene_index, motion, materials, changes);
      !content.has_value()) {
    return std::unexpected(std::move(content.error()));
  }
  const device::ChangeSet change_set = changes.Build();
  committed.description =
      Describe(committed.settings, change_set, prims.shutter);
  if (auto done = CommitToDevices(change_set, devices); !done.has_value()) {
    return std::unexpected(std::move(done.error()));
  }
  return committed;
}

}  // namespace umbra::ext::usd::internal
