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
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "host_copy.h"
#include "ies_profile.h"
#include "ies_tilt.h"
#include "instancer.h"
#include "material.h"
#include "mesh.h"
#include "motion.h"
#include "motion_samples.h"
#include "sync_counter.h"
#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/safety.h"
#include "umbra/device/render_data.h"
#include "umbra/ext/ocio/ocio.h"

namespace umbra::delegate::internal {
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

// The identifier of the shader of a PortalLight.
constexpr std::string_view kPortalLight = "PortalLight";

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
  if (type == pxr::HdPrimTypeTokens->light && shader_id != kPortalLight) {
    // Hydra gives PortalLights and GeometryLights as generic lights.
    return shader_id.empty() ? "generic light" : shader_id;
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

  // The value of the flag `name`, or none when it has no bool value.
  [[nodiscard]] std::optional<bool> Flag(const pxr::TfToken& name) const {
    const pxr::VtValue value = pxr::VtValue::Cast<bool>(Value(name));
    return value.IsHolding<bool>() ? std::optional(value.UncheckedGet<bool>())
                                   : std::nullopt;
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

host::DomeTextureFormat DomeFormat(const pxr::TfToken& format) {
  using enum host::DomeTextureFormat;
  static const std::array<std::pair<pxr::TfToken, host::DomeTextureFormat>, 4>
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

// Reads one light into its host copy, noting what it authors that umbra
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

  // Its shape, for a light of the sprim type `type`, noting a texture that
  // is not read; none for an unknown type.
  std::optional<host::Light::Shape> Shape(const pxr::TfToken& type);
  // Reads its emission into `emitter`, a light or a mesh's emission:
  // intensity, exposure, normalization, color and color temperature.
  template <typename Emitter>
  void ReadEmission(Emitter& emitter);
  // Reads its cone and its IES profile into `light`, noting its filters.
  void ReadShaping(host::Light& light);
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
  std::optional<host::Texture> Texture();
  host::Light::Dome Dome();
  // The light's IES profile; none when it has none or it is not read.
  std::optional<host::IesProfile> IesProfile();
  // The file of its IES profile, empty when it has none.
  [[nodiscard]] pxr::SdfAssetPath IesFile() const {
    return params_->Get(pxr::HdLightTokens->shapingIesFile,
                        pxr::SdfAssetPath());
  }

  const Params* params_;
  pxr::SdfPath prim_id_;
  double meters_per_unit_;
  std::vector<LightNote>* notes_;
  std::optional<Error> fatal_;
};

std::optional<host::Light::Shape> Reader::Shape(const pxr::TfToken& type) {
  const pxr::HdPrimTypeTokens_StaticTokenType& types = *pxr::HdPrimTypeTokens;
  const pxr::HdLightTokens_StaticTokenType& tokens = *pxr::HdLightTokens;
  if (type == types.sphereLight) {
    return host::Light::Sphere{.radius = Meters(tokens.radius, 0.5F)};
  }
  if (type == types.rectLight) {
    return host::Light::Rect{
        .width = Meters(tokens.width, 1.0F),
        .height = Meters(tokens.height, 1.0F),
        .texture = Texture(),
    };
  }
  if (type == types.diskLight) {
    return host::Light::Disk{.radius = Meters(tokens.radius, 0.5F)};
  }
  if (type == types.cylinderLight) {
    return host::Light::Cylinder{
        .length = Meters(tokens.length, 1.0F),
        .radius = Meters(tokens.radius, 0.5F),
    };
  }
  if (type == types.distantLight) {
    return host::Light::Distant{
        .angle = static_cast<double>(params_->Get(tokens.angle, 0.53F)),
    };
  }
  if (type == types.domeLight) {
    return Dome();
  }
  if (type == types.light) {
    // A generic light that reaches its shape is a PortalLight.
    return host::Light::Portal{
        .width = Meters(tokens.width, 1.0F),
        .height = Meters(tokens.height, 1.0F),
    };
  }
  return std::nullopt;
}

host::Light::Dome Reader::Dome() {
  host::Light::Dome dome{
      .texture = Texture(),
      .format = DomeFormat(
          params_->Get(pxr::HdLightTokens->textureFormat, pxr::TfToken())),
      .portals = {},
  };
  // A DomeLight_1 aligns its pole as its poleAxis says, with the stage's
  // up axis for "scene"; a DomeLight's pole is +Y.
  const pxr::VtValue offset = params_->Value(pxr::HdLightTokens->domeOffset);
  if (offset.IsHolding<pxr::GfMatrix4d>()) {
    dome.orientation = RowMajor(offset.UncheckedGet<pxr::GfMatrix4d>());
  }
  return dome;
}

std::optional<host::Texture> Reader::Texture() {
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
  // An emitter's texels are radiance: with no color space authored, or
  // auto, an sRGB file is sRGB and any other is scene-linear, never raw.
  pxr::TfToken authored = params_->ColorSpace(name);
  if (authored == "auto") {
    authored = pxr::TfToken();
  }
  auto color_space = TextureColorSpace(
      authored, **file, IsSrgb(**file) ? "sRGB" : kDefaultColorSpace);
  if (!color_space.has_value()) {
    Note(LightIssue::kTextureNotRead,
         std::format("{}: {}", asset.GetAssetPath(), color_space.error()));
    return std::nullopt;
  }
  return host::Texture{
      .file = std::move(**file),
      .color_space = std::move(*color_space),
  };
}

template <typename Emitter>
void Reader::ReadEmission(Emitter& emitter) {
  const pxr::HdLightTokens_StaticTokenType& tokens = *pxr::HdLightTokens;
  emitter.intensity = params_->Get(tokens.intensity, emitter.intensity);
  emitter.exposure = params_->Get(tokens.exposure, emitter.exposure);
  emitter.normalization = params_->Flag(tokens.normalize) == true
                              ? host::Normalization::kNormalized
                              : host::Normalization::kUnnormalized;
  const pxr::GfVec3f color =
      params_->Get(tokens.color, pxr::GfVec3f(1.0F, 1.0F, 1.0F));
  std::array<float, 3> rgb = {color.x, color.y, color.z};
  const pxr::TfToken space = params_->ColorSpace(tokens.color);
  if (auto converted = ext::ocio::ConvertToAcescg(
          space.IsEmpty() ? kDefaultColorSpace : space.GetString(), rgb);
      converted.has_value()) {
    emitter.color.rgb = rgb;
  } else {
    Note(LightIssue::kColorNotConverted,
         std::string(converted.error().message()));
  }
  if (params_->Flag(tokens.enableColorTemperature) != true) {
    return;
  }
  // Any temperature above 0 K is kept: UsdLux's 1000 to 10000 K is no limit
  // of a blackbody's.
  const float kelvin = params_->Get(tokens.colorTemperature, 6500.0F);
  if (kelvin > 0.0F) {
    emitter.color_temperature = kelvin;
  } else {
    Note(LightIssue::kNonPositiveTemperature, std::format("{} K", kelvin));
  }
}

void Reader::ReadShaping(host::Light& light) {
  const pxr::HdLightTokens_StaticTokenType& tokens = *pxr::HdLightTokens;
  // Without ShapingAPI a light has no cone.
  light.cone_angle = std::clamp(
      params_->Get(tokens.shapingConeAngle, light.cone_angle), 0.0F, 180.0F);
  if (!params_->Get(pxr::HdTokens->filters, pxr::SdfPathVector()).empty()) {
    Note(LightIssue::kFilters);
  }
  light.ies_profile = IesProfile();
}

std::optional<host::IesProfile> Reader::IesProfile() {
  const pxr::HdLightTokens_StaticTokenType& tokens = *pxr::HdLightTokens;
  const pxr::SdfAssetPath asset = IesFile();
  if (asset.GetAssetPath().empty()) {
    return std::nullopt;
  }
  if (asset.GetResolvedPath().empty()) {
    Note(LightIssue::kIesProfileNotRead,
         std::format("{} does not resolve", asset.GetAssetPath()));
    return std::nullopt;
  }
  auto read = ReadIesProfile(asset.GetResolvedPath());
  if (!read.has_value()) {
    Note(LightIssue::kIesProfileNotRead,
         std::format("{}: {}", asset.GetAssetPath(), read.error()));
    return std::nullopt;
  }
  if (!read->tilt_not_read.empty()) {
    Note(LightIssue::kIesTiltNotRead,
         std::format("{}: {}", asset.GetAssetPath(), read->tilt_not_read));
  }
  host::IesProfile& profile = read->profile;
  profile.normalization = params_->Flag(tokens.shapingIesNormalize) == true
                              ? host::Normalization::kNormalized
                              : host::Normalization::kUnnormalized;
  return std::move(profile);
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
  // The angle scale has no effect without a profile.
  if (params_->Get(tokens.shapingIesAngleScale, 0.0F) != 0.0F &&
      !IesFile().GetAssetPath().empty()) {
    Note(LightIssue::kIesAngleScale);
  }
  const pxr::GfVec3f black(0.0F);
  if (params_->Flag(tokens.shadowEnable) == false ||
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

namespace {

// The transform whose motion samples, equally spaced over `shutter`, are
// `motion`, at the frame: at the end of the shutter nearest to the frame
// when the shutter does not span it.
pxr::GfMatrix4d AtFrame(const TransformMotion& motion,
                        const host::Shutter& shutter) {
  if (motion.empty()) {
    return pxr::GfMatrix4d(1.0);
  }
  if (motion.size() == 1 || !IsMotionBlurred(shutter)) {
    return motion.front();
  }
  const double weight =
      std::clamp(-shutter.open / (shutter.close - shutter.open), 0.0, 1.0);
  const double position = weight * static_cast<double>(motion.size() - 1);
  const std::size_t index =
      std::min(static_cast<std::size_t>(position), motion.size() - 2);
  return BlendTransforms(motion.at(index), motion.at(index + 1),
                         position - static_cast<double>(index));
}

// The tilt factor of each of `instances`, sampled over `shutter`, of a
// light whose IES profile is `profile`, on a stage whose up axis is
// `up_axis`: 1 for each when the profile has no TILT data.
host::HostBuffer<float> TiltFactors(
    std::span<const FlatInstance> instances,
    const std::optional<host::IesProfile>& profile,
    const host::Shutter& shutter, UpAxis up_axis) {
  host::HostBuffer<float> factors;
  if (!profile.has_value() || !profile->tilt.has_value()) {
    factors.assign(instances.size(), 1.0F);
    return factors;
  }
  factors.reserve(instances.size());
  for (const FlatInstance& instance : instances) {
    factors.push_back(IesTiltFactor(
        *profile->tilt,
        IesInclination(RowMajor(AtFrame(instance.transforms, shutter)),
                       up_axis)));
  }
  return factors;
}

// Whether the mesh at `prim_id` is a mesh light: an emissive mesh, its
// emission authored by UsdLux's MeshLightAPI.
bool IsMeshLight(pxr::HdSceneDelegate& delegate, const pxr::SdfPath& prim_id) {
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

}  // namespace

std::optional<host::Emission> ReadMeshLight(pxr::HdSceneDelegate& delegate,
                                            const pxr::SdfPath& prim_id,
                                            std::vector<LightNote>& notes) {
  if (!IsMeshLight(delegate, prim_id)) {
    return std::nullopt;
  }
  const Params params(delegate, prim_id, PrimDataSource(delegate, prim_id));
  // The emission has no length: the scale of meters is never read.
  Reader reader(params, prim_id, 1.0, notes);
  host::Emission emission;
  reader.ReadEmission(emission);
  reader.NoteArtisticControls();
  return emission;
}

LightPrim::LightPrim(pxr::TfToken type_id, const pxr::SdfPath& prim_id,
                     SyncCounter* UMBRA_NULLABLE counter,
                     double meters_per_unit, UpAxis up_axis)
    : pxr::HdLight(prim_id),
      type_(std::move(type_id)),
      counter_(counter),
      meters_per_unit_(meters_per_unit),
      up_axis_(up_axis) {}

pxr::HdDirtyBits LightPrim::GetInitialDirtyBitsMask() const {
  return pxr::HdLight::AllDirty;
}

void LightPrim::Sync(pxr::HdSceneDelegate* scene_delegate,
                     pxr::HdRenderParam* UMBRA_NULLABLE /*render_param*/,
                     pxr::HdDirtyBits* dirty_bits) {
  if (counter_ == nullptr) {
    *dirty_bits = pxr::HdLight::Clean;
    return;
  }
  _UpdateInstancer(scene_delegate, dirty_bits);
  *dirty_bits = pxr::HdLight::Clean;
  delegate_ = scene_delegate;
  Read(*scene_delegate);
  counter_->Add(1);
  if (state_ == LightState::kConverted) {
    UMBRA_LOG_TRACE(kUsd, "synced {} ({} light)", GetId().GetString(),
                    device::kLightShapeNames.at(light_.shape.index()));
  } else {
    UMBRA_LOG_TRACE(kUsd, "synced {}", GetId().GetString());
  }
}

void LightPrim::Read(pxr::HdSceneDelegate& delegate) {
  changed_ = Changed::kYes;
  state_ = LightState::kHidden;
  instance_count_ = 0;
  notes_.clear();
  fatal_.reset();
  portals_.clear();
  light_ = {};
  role_ = type_ == pxr::HdPrimTypeTokens->light &&
                  ShaderId(PrimDataSource(delegate, GetId())) == kPortalLight
              ? Role::kPortal
              : Role::kEmitter;
  render_tag_ = delegate.GetRenderTag(GetId());
  if (delegate.GetVisible(GetId())) {
    Convert(delegate);
  }
}

void LightPrim::ReadAgain(const host::Shutter& shutter,
                          const pxr::TfTokenVector& render_tags) {
  if (delegate_ == nullptr || IsChanged()) {
    return;
  }
  Read(*delegate_);
  Place(shutter, render_tags);
  UMBRA_LOG_TRACE(kUsd, "read {} again", GetId().GetString());
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
  std::optional<host::Light::Shape> shape = reader.Shape(type_);
  if (!shape.has_value()) {
    return;
  }
  light_.path = GetId().GetString();
  light_.shape = std::move(*shape);
  state_ = LightState::kConverted;
  if (IsPortal()) {
    // A portal emits nothing: it keeps the default emission.
    return;
  }
  if (type_ == pxr::HdPrimTypeTokens->domeLight) {
    portals_ = params.Get(pxr::HdTokens->portals, pxr::SdfPathVector());
  }
  reader.ReadEmission(light_);
  reader.ReadShaping(light_);
  reader.NoteArtisticControls();
  fatal_ = reader.TakeFatal();
}

void LightPrim::Place(const host::Shutter& shutter,
                      const pxr::TfTokenVector& render_tags) {
  if (state_ == LightState::kHidden || delegate_ == nullptr) {
    return;
  }
  if (std::ranges::find(render_tags, render_tag_) == render_tags.end()) {
    state_ = LightState::kHidden;
    instance_count_ = 0;
    return;
  }
  if (state_ != LightState::kConverted) {
    return;
  }
  const Placement placement =
      PlaceInstances(*delegate_, GetId(), GetInstancerId(), shutter);
  motion_outcome_ = placement.outcome;
  instance_count_ = placement.instances.size();
  light_.instances.clear();
  light_.instances.reserve(placement.instances.size());
  for (const FlatInstance& instance : placement.instances) {
    light_.instances.push_back({
        .object_to_world =
            TransformsInMeters(instance.transforms, meters_per_unit_),
        .primvars = {},
    });
  }
  light_.tilt_factors =
      TiltFactors(placement.instances, light_.ies_profile, shutter, up_axis_);
  if (light_.ies_profile.has_value() && light_.ies_profile->tilt.has_value()) {
    UMBRA_LOG_TRACE(kUsd, "placed {} with the tilt factors {}",
                    GetId().GetString(), light_.tilt_factors);
  }
}

void LightPrim::AssignPortals(std::span<const device::LightId> ids) {
  if (auto* dome = std::get_if<host::Light::Dome>(&light_.shape)) {
    dome->portals.assign(ids.begin(), ids.end());
  }
}

void LightPrim::FreeHostCopy() {
  light_ = {};
  changed_ = Changed::kNo;
}

}  // namespace umbra::delegate::internal
