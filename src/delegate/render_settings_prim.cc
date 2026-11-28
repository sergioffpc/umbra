// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "render_settings_prim.h"

#include <pxr/base/gf/range2f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderSettings.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdRender/tokens.h>
#include <pxr/usd/usdRender/var.h>
#include <pxr/usdImaging/usdImaging/usdRenderProductSchema.h>
#include <pxr/usdImaging/usdImaging/usdRenderSettingsSchema.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "gf.h"
#include "scene_index.h"
#include "sync_counter.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/device/image.h"

namespace umbra::delegate::internal {
namespace {

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

// The name of the samples per pixel on a RenderSettings or RenderProduct
// prim.
const pxr::TfToken& SamplesPerPixelName() {
  static const pxr::TfToken kName("umbra:samplesPerPixel");
  return kName;
}

// The namespaced setting `name` of `settings`, a prim's namespacedSettings
// in the terminal scene index, or none.
pxr::HdSampledDataSourceHandle Namespaced(
    const pxr::HdContainerDataSourceHandle& settings,
    const pxr::TfToken& name) {
  return pxr::HdSampledDataSource::Cast(
      settings != nullptr ? settings->Get(name) : nullptr);
}

// umbra:dicingRate, from the prim's UsdRender data source in the terminal
// scene index.
DicingRate ReadDicingRate(const pxr::HdContainerDataSourceHandle& prim) {
  const auto source =
      Namespaced(pxr::UsdImagingUsdRenderSettingsSchema::GetFromParent(prim)
                     .GetNamespacedSettings(),
                 DicingRateName());
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

// A data window as xmin, ymin, xmax, ymax.
std::array<float, 4> DataWindow(const pxr::GfRange2f& window) {
  const std::array<float, 2> min = Components<float, 2>(window.GetMin());
  const std::array<float, 2> max = Components<float, 2>(window.GetMax());
  return {min.at(0), min.at(1), max.at(0), max.at(1)};
}

// The image of a product of the framing `base`, before its data window is
// resolved into pixels.
device::Image BaseImage(const SettingsBase& base) {
  return {
      .resolution = base.resolution,
      .window = {},
      .pixel_aspect_ratio = base.pixel_aspect_ratio,
      .aspect_ratio_conform_policy =
          ConformPolicy(base.aspect_ratio_conform_policy),
  };
}

// umbra:samplesPerPixel, from `settings`, a prim's namespacedSettings in the
// terminal scene index, at the frame.
AuthoredSamples ReadSamplesPerPixel(
    const pxr::HdContainerDataSourceHandle& settings) {
  const auto source = Namespaced(settings, SamplesPerPixelName());
  if (source == nullptr) {
    return {};
  }
  const pxr::VtValue value = source->GetValue(0.0F);
  if (value.IsEmpty()) {
    return {};
  }
  if (!value.IsHolding<int>() || value.UncheckedGet<int>() <= 0) {
    return {.samples = std::nullopt, .validity = SamplesValidity::kInvalid};
  }
  return {
      .samples = static_cast<std::uint32_t>(value.UncheckedGet<int>()),
      .validity = SamplesValidity::kValid,
  };
}

// Whether `data_type` has half components: half, half3, color3h, ...
bool IsHalfType(std::string_view data_type) {
  return data_type.starts_with("half") || data_type.ends_with('h');
}

// The channel type of `var`: see ProductAttributes::var_types. Whether its
// prim authors its dataType is read from `stage`, as Hydra does not tell.
ChannelType ChannelTypeOf(
    const pxr::HdRenderSettings::RenderProduct::RenderVar& var,
    const pxr::UsdStage* UMBRA_NULLABLE stage) {
  const bool authored =
      stage != nullptr && pxr::UsdRenderVar(stage->GetPrimAtPath(var.varPath))
                              .GetDataTypeAttr()
                              .HasAuthoredValue();
  return authored && !IsHalfType(var.dataType.GetString()) ? ChannelType::kFloat
                                                           : ChannelType::kHalf;
}

// What the settings read of `product`, whose prim in the terminal scene
// index, `prim`, authors its samples per pixel.
ProductAttributes ReadProductAttributes(
    const pxr::HdRenderSettings::RenderProduct& product,
    const pxr::HdContainerDataSourceHandle& prim,
    const pxr::UsdStage* UMBRA_NULLABLE stage) {
  ProductAttributes attributes{
      .samples_per_pixel = ReadSamplesPerPixel(
          pxr::UsdImagingUsdRenderProductSchema::GetFromParent(prim)
              .GetNamespacedSettings()),
      .var_types = {},
  };
  attributes.var_types.reserve(product.renderVars.size());
  for (const pxr::HdRenderSettings::RenderProduct::RenderVar& var :
       product.renderVars) {
    attributes.var_types.push_back(ChannelTypeOf(var, stage));
  }
  return attributes;
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

SettingsBase ProductBase(const pxr::HdRenderSettings::RenderProduct& product) {
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

RenderSettingsPrim::RenderSettingsPrim(
    const pxr::SdfPath& prim_id, SyncCounter* UMBRA_NULLABLE counter,
    const pxr::UsdStage* UMBRA_NULLABLE stage)
    : pxr::HdRenderSettings(prim_id), counter_(counter), stage_(stage) {}

void RenderSettingsPrim::_Sync(
    pxr::HdSceneDelegate* scene_delegate,
    pxr::HdRenderParam* UMBRA_NULLABLE /*render_param*/,
    const pxr::HdDirtyBits* /*dirty_bits*/) {
  const pxr::HdContainerDataSourceHandle prim =
      PrimDataSource(*scene_delegate, GetId());
  base_ = ReadSettingsBase(prim);
  dicing_rate_ = ReadDicingRate(prim);
  samples_per_pixel_ = ReadSamplesPerPixel(
      pxr::UsdImagingUsdRenderSettingsSchema::GetFromParent(prim)
          .GetNamespacedSettings());
  product_attributes_.clear();
  product_attributes_.reserve(GetRenderProducts().size());
  for (const pxr::HdRenderSettings::RenderProduct& product :
       GetRenderProducts()) {
    product_attributes_.push_back(ReadProductAttributes(
        product, PrimDataSource(*scene_delegate, product.productPath), stage_));
  }
  CountSync(counter_, GetId());
}

pxr::SdfPath RenderSettingsPrim::CameraPath() const {
  const pxr::VtValue& camera = GetCamera();
  return camera.IsHolding<pxr::SdfPath>() ? camera.UncheckedGet<pxr::SdfPath>()
                                          : pxr::SdfPath();
}

std::vector<device::Image> ImagesOf(const RenderSettingsPrim& settings) {
  std::vector<device::Image> images;
  for (const pxr::HdRenderSettings::RenderProduct& product :
       settings.GetRenderProducts()) {
    if (product.cameraPath == settings.CameraPath()) {
      images.push_back(BaseImage(ProductBase(product)));
    }
  }
  if (images.empty()) {
    images.push_back(BaseImage(settings.base()));
  }
  return images;
}

}  // namespace umbra::delegate::internal
