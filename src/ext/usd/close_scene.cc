// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "close_scene.h"

#include <pxr/base/gf/range1f.h>
#include <pxr/base/gf/range2f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/renderSettings.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdRender/tokens.h>
#include <pxr/usd/usdRender/var.h>

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
#include "mesh.h"
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

// The cameras the settings and their products see through, converted once
// each, in meters.
class Cameras {
 public:
  Cameras(const SyncedPrims& prims UMBRA_LIFETIMEBOUND, double meters_per_unit)
      : prims_(&prims), meters_per_unit_(meters_per_unit) {}

  // The index of the camera at `path`, which `owner` names: fails when
  // there is none, or it is not a Camera.
  std::expected<std::size_t, Error> Index(const pxr::SdfPath& path,
                                          std::string_view owner);

  std::vector<scene::Camera> Take() { return std::move(cameras_); }

 private:
  [[nodiscard]] scene::Camera Convert(const CameraPrim& prim) const;
  // A length in scene units, in meters.
  [[nodiscard]] double Meters(float length) const {
    return static_cast<double>(length) * meters_per_unit_;
  }

  const SyncedPrims* prims_;
  double meters_per_unit_;
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

scene::Camera Cameras::Convert(const CameraPrim& prim) const {
  const double scale = meters_per_unit_;
  // Lengths are in scene units, the apertures and focal length too once
  // Hydra has read them.
  scene::Camera camera;
  camera.path = prim.GetId().GetString();
  camera.projection = prim.GetProjection() == pxr::HdCamera::Orthographic
                          ? scene::Projection::kOrthographic
                          : scene::Projection::kPerspective;
  camera.camera_to_world = TransformInMeters(prim.GetTransform(), scale);
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
  camera.shutter = {prim.GetShutterOpen(), prim.GetShutterClose()};
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

// "1 mesh asks", "120 meshes ask".
std::string Meshes(std::size_t count, std::string_view singular_verb,
                   std::string_view plural_verb) {
  return count == 1 ? std::format("1 mesh {}", singular_verb)
                    : std::format("{} meshes {}", count, plural_verb);
}

// The meshes the Scene takes, as prototypes with one instance each, and the
// tallies of those it does not, for their warnings.
class Geometry {
 public:
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
  std::vector<scene::Mesh> prototypes_;
  std::vector<scene::Instance> instances_;
  std::map<std::string, Tally> subdivided_;
  Tally invalid_;
  Tally instanced_;
  std::map<pxr::TfToken, Tally> unsupported_;

  void ReportMeshes() const;
  void ReportUnsupported() const;
};

void Geometry::Add(const pxr::SdfPath& path, MeshPrim& mesh) {
  switch (mesh.state()) {
    case MeshState::kNotSynced:
    case MeshState::kInvisible:
      return;
    case MeshState::kInstanced:
      instanced_.Add(path);
      return;
    case MeshState::kInvalid:
      invalid_.Add(path, mesh.invalid_reason());
      return;
    case MeshState::kConverted:
      break;
  }
  if (!mesh.subdivision_scheme().empty()) {
    subdivided_[mesh.subdivision_scheme()].Add(path);
  }
  instances_.push_back({
      .prototype = prototypes_.size(),
      .object_to_world = {mesh.object_to_world()},
      .light_link = scene::kAllLights,
      .shadow_link = scene::kAllLights,
  });
  prototypes_.push_back(mesh.TakeMesh());
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
  if (instanced_.count() > 0) {
    UMBRA_LOG_WARNING(kUsd, "{} instanced, not supported yet; left out ({})",
                      Meshes(instanced_.count(), "is", "are"),
                      instanced_.First());
  }
}

void Geometry::ReportUnsupported() const {
  for (const auto& [type, tally] : unsupported_) {
    UMBRA_LOG_WARNING(kUsd, "{} {} {} not supported yet; left out ({})",
                      tally.count(), type.GetString(),
                      Plural(tally.count(), "prim", "prims"), tally.First());
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

// The camera the settings name, an empty path for none.
pxr::SdfPath SettingsCamera(const RenderSettingsPrim& settings) {
  const pxr::VtValue& camera = settings.GetCamera();
  return camera.IsHolding<pxr::SdfPath>() ? camera.UncheckedGet<pxr::SdfPath>()
                                          : pxr::SdfPath();
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

}  // namespace

std::expected<scene::Scene, Error> CloseScene(const CloseInputs& inputs,
                                              const pxr::UsdStage& stage,
                                              const SyncedPrims& prims) {
  const auto found = prims.settings.find(inputs.settings);
  if (found == prims.settings.end()) {
    return std::unexpected(
        Error(ErrorCode::kNotFound, std::format("no RenderSettings prim at {}",
                                                inputs.settings.GetString())));
  }
  const RenderSettingsPrim& settings = *found->second;
  Cameras cameras(prims, inputs.meters_per_unit);
  const auto camera = cameras.Index(
      SettingsCamera(settings),
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
  Geometry geometry;
  for (const auto& [path, mesh] : prims.meshes) {
    geometry.Add(path, *mesh);
  }
  for (const auto& [path, prim] : prims.unsupported) {
    geometry.Add(*prim);
  }
  geometry.Report();
  return scene::Scene(std::move(converted), cameras.Take(),
                      geometry.TakePrototypes(), geometry.TakeInstances());
}

}  // namespace umbra::ext::usd::internal
