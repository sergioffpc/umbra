// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "render_settings_prim.h"

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderSettings.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usdRender/tokens.h>
#include <pxr/usdImaging/usdImaging/usdRenderSettingsSchema.h>

#include <cmath>
#include <cstdint>
#include <memory>
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
  const pxr::HdContainerDataSourceHandle prim =
      PrimDataSource(*scene_delegate, GetId());
  base_ = ReadSettingsBase(prim);
  dicing_rate_ = ReadDicingRate(prim);
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

}  // namespace umbra::delegate::internal
