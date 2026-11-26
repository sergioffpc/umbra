// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "light.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/dataSourceTypeDefs.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/materialNetworkSchema.h>
#include <pxr/imaging/hd/materialNodeParameterSchema.h>
#include <pxr/imaging/hd/materialNodeSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/schemaTypeDefs.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "instancer.h"
#include "material.h"
#include "mesh.h"
#include "motion.h"
#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/progress.h"
#include "umbra/core/safety.h"
#include "umbra/ext/ocio/ocio.h"
#include "umbra/scene/scene.h"

namespace umbra::ext::usd::internal {
namespace {

using ::umbra::core::Error;

// The data source of the prim at `prim_id` in the terminal scene index, or
// none.
pxr::HdContainerDataSourceHandle PrimDataSource(pxr::HdSceneDelegate& delegate,
                                                const pxr::SdfPath& prim_id) {
  const pxr::HdSceneIndexBaseRefPtr terminal =
      delegate.GetRenderIndex().GetTerminalSceneIndex();
  return terminal == nullptr ? nullptr : terminal->GetPrim(prim_id).dataSource;
}

// The node of the light's material network: UsdLux's shader, whose
// parameters carry the color spaces of the light's inputs.
pxr::HdMaterialNodeSchema LightNode(
    const pxr::HdContainerDataSourceHandle& prim) {
  const pxr::HdMaterialNodeContainerSchema nodes =
      pxr::HdMaterialSchema::GetFromParent(prim)
          .GetMaterialNetwork()
          .GetNodes();
  const pxr::TfTokenVector names = nodes.GetNames();
  if (names.empty()) {
    return {nullptr};
  }
  return nodes.Get(names.front());
}

// The identifier of the light's shader ("PortalLight"), or empty.
std::string ShaderId(const pxr::HdContainerDataSourceHandle& prim) {
  const pxr::HdTokenDataSourceHandle identifier =
      LightNode(prim).GetNodeIdentifier();
  return identifier == nullptr ? std::string()
                               : identifier->GetTypedValue(0.0F).GetString();
}

// The UsdLux type of a light of the sprim type `type`, whose shader is
// `shader_id`, that umbra does not take; empty for one it takes.
std::string UnsupportedType(const pxr::TfToken& type,
                            const std::string& shader_id) {
  if (type == pxr::HdPrimTypeTokens->pluginLight) {
    return "PluginLight";
  }
  if (type == pxr::HdPrimTypeTokens->light) {
    // Hydra gives PortalLights and GeometryLights as generic lights.
    return shader_id.empty() ? "light" : shader_id;
  }
  return {};
}

// The parameters of one light, as Hydra gives them.
class Params {
 public:
  Params(pxr::HdSceneDelegate& delegate UMBRA_LIFETIMEBOUND,
         const pxr::SdfPath& prim_id UMBRA_LIFETIMEBOUND,
         pxr::HdContainerDataSourceHandle prim)
      : delegate_(&delegate), prim_id_(&prim_id), prim_(std::move(prim)) {}

  [[nodiscard]] pxr::VtValue Value(const pxr::TfToken& name) const {
    return delegate_->GetLightParamValue(*prim_id_, name);
  }

  // The value of `name` as a T, or `fallback` when it has none of a type
  // that casts to T.
  template <typename T>
  [[nodiscard]] T Get(const pxr::TfToken& name, const T& fallback) const {
    const pxr::VtValue value = pxr::VtValue::Cast<T>(Value(name));
    return value.IsHolding<T>() ? value.UncheckedGet<T>() : fallback;
  }

  // The color space authored on the input `name`, or empty.
  [[nodiscard]] pxr::TfToken ColorSpace(const pxr::TfToken& name) const {
    const pxr::HdTokenDataSourceHandle space =
        LightNode(prim_).GetParameters().Get(name).GetColorSpace();
    return space == nullptr ? pxr::TfToken() : space->GetTypedValue(0.0F);
  }

 private:
  pxr::HdSceneDelegate* delegate_;
  const pxr::SdfPath* prim_id_;
  pxr::HdContainerDataSourceHandle prim_;
};

// A transform as the Scene holds it, row-major, with no unit.
std::array<double, 16> RowMajor(const pxr::GfMatrix4d& transform) {
  return TransformInMeters(transform, 1.0);
}

scene::DomeTextureFormat DomeFormat(const pxr::TfToken& format) {
  using enum scene::DomeTextureFormat;
  static const std::array<std::pair<pxr::TfToken, scene::DomeTextureFormat>, 4>
      kFormats = {
          {
              {pxr::TfToken("latlong"), kLatlong},
              {pxr::TfToken("mirroredBall"), kMirroredBall},
              {pxr::TfToken("angular"), kAngular},
              {pxr::TfToken("cubeMapVerticalCross"), kCubeMapVerticalCross},
          },
  };
  for (const auto& [token, value] : kFormats) {
    if (token == format) {
      return value;
    }
  }
  return kAutomatic;
}

// Reads one light into the Scene's, noting what it authors that umbra
// does not take.
class Reader {
 public:
  Reader(const Params& params UMBRA_LIFETIMEBOUND, pxr::SdfPath prim_id,
         double meters_per_unit,
         std::vector<LightNote>& notes UMBRA_LIFETIMEBOUND)
      : params_(&params),
        prim_id_(std::move(prim_id)),
        meters_per_unit_(meters_per_unit),
        notes_(&notes) {}

  // Its shape, for a light of the sprim type `type`; none for an unknown
  // type.
  std::optional<scene::Light::Shape> Shape(const pxr::TfToken& type);
  void Emission(scene::Light& light);
  void Shaping(scene::Light& light);
  // Notes the controls umbra does not take
  // (docs/adr/0010-lights-are-physical.md).
  void NoteArtisticControls();

  [[nodiscard]] std::optional<Error> TakeFatal() { return std::move(fatal_); }

 private:
  [[nodiscard]] double Meters(const pxr::TfToken& name, float fallback) const {
    return static_cast<double>(params_->Get(name, fallback)) * meters_per_unit_;
  }
  void Note(LightIssue issue, std::string detail = {}) {
    notes_->push_back({.issue = issue, .detail = std::move(detail)});
  }
  // The light's texture, resolved with its color space; none when it has
  // none or it is not read.
  std::optional<scene::Texture> Texture();
  scene::Light::Dome Dome();

  const Params* params_;
  pxr::SdfPath prim_id_;
  double meters_per_unit_;
  std::vector<LightNote>* notes_;
  std::optional<Error> fatal_;
};

std::optional<scene::Light::Shape> Reader::Shape(const pxr::TfToken& type) {
  const pxr::HdPrimTypeTokens_StaticTokenType& types = *pxr::HdPrimTypeTokens;
  const pxr::HdLightTokens_StaticTokenType& tokens = *pxr::HdLightTokens;
  if (type == types.sphereLight) {
    return scene::Light::Sphere{.radius = Meters(tokens.radius, 0.5F)};
  }
  if (type == types.rectLight) {
    return scene::Light::Rect{
        .width = Meters(tokens.width, 1.0F),
        .height = Meters(tokens.height, 1.0F),
        .texture = Texture(),
    };
  }
  if (type == types.diskLight) {
    return scene::Light::Disk{.radius = Meters(tokens.radius, 0.5F)};
  }
  if (type == types.cylinderLight) {
    return scene::Light::Cylinder{
        .length = Meters(tokens.length, 1.0F),
        .radius = Meters(tokens.radius, 0.5F),
    };
  }
  if (type == types.distantLight) {
    return scene::Light::Distant{
        .angle = static_cast<double>(params_->Get(tokens.angle, 0.53F)),
    };
  }
  if (type == types.domeLight) {
    return Dome();
  }
  return std::nullopt;
}

scene::Light::Dome Reader::Dome() {
  scene::Light::Dome dome{
      .texture = Texture(),
      .format = DomeFormat(
          params_->Get(pxr::HdLightTokens->textureFormat, pxr::TfToken())),
  };
  // A DomeLight_1 aligns its pole as its poleAxis says, with the stage's
  // up axis for "scene"; a DomeLight's pole is +Y.
  const pxr::VtValue offset = params_->Value(pxr::HdLightTokens->domeOffset);
  if (offset.IsHolding<pxr::GfMatrix4d>()) {
    dome.orientation = RowMajor(offset.UncheckedGet<pxr::GfMatrix4d>());
  }
  return dome;
}

std::optional<scene::Texture> Reader::Texture() {
  const pxr::TfToken& name = pxr::HdLightTokens->textureFile;
  const auto asset = params_->Get(name, pxr::SdfAssetPath());
  if (asset.GetAssetPath().empty()) {
    return std::nullopt;
  }
  auto file = ResolveAsset(asset, prim_id_);
  if (!file.has_value()) {
    if (!fatal_.has_value()) {
      fatal_ = std::move(file.error());
    }
    return std::nullopt;
  }
  if (!file->has_value()) {
    Note(LightIssue::kTextureNotRead,
         std::format("{} does not resolve", asset.GetAssetPath()));
    return std::nullopt;
  }
  auto color_space =
      TextureColorSpace(params_->ColorSpace(name), **file, kDefaultColorSpace);
  if (!color_space.has_value()) {
    Note(LightIssue::kTextureNotRead,
         std::format("{}: {}", asset.GetAssetPath(), color_space.error()));
    return std::nullopt;
  }
  return scene::Texture{
      .file = std::move(**file),
      .color_space = std::move(*color_space),
  };
}

void Reader::Emission(scene::Light& light) {
  const pxr::HdLightTokens_StaticTokenType& tokens = *pxr::HdLightTokens;
  light.intensity = params_->Get(tokens.intensity, light.intensity);
  light.exposure = params_->Get(tokens.exposure, light.exposure);
  light.normalization = params_->Get(tokens.normalize, false)
                            ? scene::Normalization::kNormalized
                            : scene::Normalization::kUnnormalized;
  const pxr::GfVec3f color =
      params_->Get(tokens.color, pxr::GfVec3f(1.0F, 1.0F, 1.0F));
  std::array<float, 3> rgb = {color.x, color.y, color.z};
  const pxr::TfToken space = params_->ColorSpace(tokens.color);
  if (auto converted = ocio::ConvertToAcescg(
          space.IsEmpty() ? kDefaultColorSpace : space.GetString(), rgb);
      converted.has_value()) {
    light.color.rgb = rgb;
  } else {
    Note(LightIssue::kColorNotConverted,
         std::string(converted.error().message()));
  }
  if (!params_->Get(tokens.enableColorTemperature, false)) {
    return;
  }
  // Any temperature above 0 K is kept: UsdLux's 1000 to 10000 K is no limit
  // of a blackbody's.
  const float kelvin = params_->Get(tokens.colorTemperature, 6500.0F);
  if (kelvin > 0.0F) {
    light.color_temperature = kelvin;
  } else {
    Note(LightIssue::kNonPositiveTemperature, std::format("{} K", kelvin));
  }
}

void Reader::Shaping(scene::Light& light) {
  const pxr::HdLightTokens_StaticTokenType& tokens = *pxr::HdLightTokens;
  // Without ShapingAPI a light has no cone.
  light.cone_angle = std::clamp(
      params_->Get(tokens.shapingConeAngle, light.cone_angle), 0.0F, 180.0F);
  if (!params_->Get(pxr::HdTokens->filters, pxr::SdfPathVector()).empty()) {
    Note(LightIssue::kFilters);
  }
  if (!params_->Get(tokens.shapingIesFile, pxr::SdfAssetPath())
           .GetAssetPath()
           .empty()) {
    Note(LightIssue::kIesProfile);
  }
}

void Reader::NoteArtisticControls() {
  const pxr::HdLightTokens_StaticTokenType& tokens = *pxr::HdLightTokens;
  if (params_->Get(tokens.diffuse, 1.0F) != 1.0F ||
      params_->Get(tokens.specular, 1.0F) != 1.0F) {
    Note(LightIssue::kDiffuseSpecular);
  }
  if (params_->Get(tokens.shapingFocus, 0.0F) != 0.0F) {
    Note(LightIssue::kFocus);
  }
  // Softness has no effect without a cone.
  if (params_->Get(tokens.shapingConeSoftness, 0.0F) != 0.0F &&
      params_->Get(tokens.shapingConeAngle, 180.0F) < 180.0F) {
    Note(LightIssue::kConeSoftness);
  }
  const pxr::GfVec3f black(0.0F);
  if (!params_->Get(tokens.shadowEnable, true) ||
      params_->Get(tokens.shadowColor, black) != black ||
      params_->Get(tokens.shadowDistance, -1.0F) != -1.0F ||
      params_->Get(tokens.shadowFalloff, -1.0F) != -1.0F ||
      params_->Get(tokens.shadowFalloffGamma, 1.0F) != 1.0F) {
    Note(LightIssue::kShadowControls);
  }
}

}  // namespace

const pxr::TfTokenVector& LightPrimTypes() {
  static const pxr::TfTokenVector kTypes = {
      pxr::HdPrimTypeTokens->sphereLight,  pxr::HdPrimTypeTokens->rectLight,
      pxr::HdPrimTypeTokens->diskLight,    pxr::HdPrimTypeTokens->cylinderLight,
      pxr::HdPrimTypeTokens->distantLight, pxr::HdPrimTypeTokens->domeLight,
      pxr::HdPrimTypeTokens->light,        pxr::HdPrimTypeTokens->pluginLight,
  };
  return kTypes;
}

bool IsLight(pxr::HdSceneDelegate& delegate, const pxr::SdfPath& prim_id) {
  const pxr::HdContainerDataSourceHandle light =
      pxr::HdLightSchema::GetFromParent(PrimDataSource(delegate, prim_id))
          .GetContainer();
  if (light == nullptr) {
    return false;
  }
  const pxr::HdBoolDataSourceHandle is_light =
      pxr::HdBoolDataSource::Cast(light->Get(pxr::HdTokens->isLight));
  return is_light != nullptr && is_light->GetTypedValue(0.0F);
}

LightPrim::LightPrim(pxr::TfToken type_id, const pxr::SdfPath& prim_id,
                     core::ProgressPhase* UMBRA_NULLABLE phase,
                     double meters_per_unit)
    : pxr::HdLight(prim_id),
      type_(std::move(type_id)),
      phase_(phase),
      meters_per_unit_(meters_per_unit) {}

pxr::HdDirtyBits LightPrim::GetInitialDirtyBitsMask() const {
  return pxr::HdLight::AllDirty;
}

void LightPrim::Sync(pxr::HdSceneDelegate* scene_delegate,
                     pxr::HdRenderParam* UMBRA_NULLABLE /*render_param*/,
                     pxr::HdDirtyBits* dirty_bits) {
  if (phase_ == nullptr) {
    *dirty_bits = pxr::HdLight::Clean;
    return;
  }
  _UpdateInstancer(scene_delegate, dirty_bits);
  *dirty_bits = pxr::HdLight::Clean;
  delegate_ = scene_delegate;
  render_tag_ = scene_delegate->GetRenderTag(GetId());
  if (scene_delegate->GetVisible(GetId())) {
    Convert(*scene_delegate);
  }
  phase_->Add(1);
  if (state_ == LightState::kConverted) {
    UMBRA_LOG_TRACE(kUsd, "synced {} ({} light)", GetId().GetString(),
                    scene::kLightShapeNames.at(light_.shape.index()));
  } else {
    UMBRA_LOG_TRACE(kUsd, "synced {}", GetId().GetString());
  }
}

void LightPrim::Convert(pxr::HdSceneDelegate& delegate) {
  const pxr::HdContainerDataSourceHandle prim =
      PrimDataSource(delegate, GetId());
  unsupported_type_ = UnsupportedType(type_, ShaderId(prim));
  if (!unsupported_type_.empty()) {
    state_ = LightState::kUnsupported;
    return;
  }
  const Params params(delegate, GetId(), prim);
  Reader reader(params, GetId(), meters_per_unit_, notes_);
  std::optional<scene::Light::Shape> shape = reader.Shape(type_);
  if (!shape.has_value()) {
    return;
  }
  light_.path = GetId().GetString();
  light_.shape = std::move(*shape);
  reader.Emission(light_);
  reader.Shaping(light_);
  reader.NoteArtisticControls();
  fatal_ = reader.TakeFatal();
  state_ = LightState::kConverted;
}

void LightPrim::Place(const scene::Shutter& shutter,
                      const pxr::TfTokenVector& render_tags) {
  if (state_ == LightState::kHidden || delegate_ == nullptr) {
    return;
  }
  if (std::ranges::find(render_tags, render_tag_) == render_tags.end()) {
    state_ = LightState::kHidden;
    return;
  }
  if (state_ != LightState::kConverted) {
    return;
  }
  const Placement placement =
      PlaceInstances(*delegate_, GetId(), GetInstancerId(), shutter);
  motion_outcome_ = placement.outcome;
  lights_.clear();
  lights_.reserve(placement.instances.size());
  for (const FlatInstance& instance : placement.instances) {
    scene::Light light = light_;
    light.light_to_world.reserve(instance.transforms.size());
    for (const pxr::GfMatrix4d& transform : instance.transforms) {
      light.light_to_world.push_back(
          TransformInMeters(transform, meters_per_unit_));
    }
    lights_.push_back(std::move(light));
  }
}

std::vector<scene::Light> LightPrim::TakeLights() { return std::move(lights_); }

}  // namespace umbra::ext::usd::internal
