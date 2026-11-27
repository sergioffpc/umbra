// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// A RenderSettings prim, as Hydra gives it, synced: the attributes its
// products inherit, as UsdRender resolves them, its dicing rate, and the
// images of the products that see through its camera.

#ifndef UMBRA_DELEGATE_RENDER_SETTINGS_PRIM_H_
#define UMBRA_DELEGATE_RENDER_SETTINGS_PRIM_H_

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderSettings.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sync_counter.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/device/image.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

namespace pxr = PXR_NS;

// The components of a Gf vector of 2 or 4 (GfVec2i, GfVec4d, ...), as T,
// read from its named fields.
template <typename T, std::size_t N, typename Vector>
std::array<T, N> Components(const Vector& vector) {
  static_assert(Vector::dimension == N);
  if constexpr (N == 2) {
    return {
        static_cast<T>(vector.x),
        static_cast<T>(vector.y),
    };
  } else {
    static_assert(N == 4);
    return {
        static_cast<T>(vector.x),
        static_cast<T>(vector.y),
        static_cast<T>(vector.z),
        static_cast<T>(vector.w),
    };
  }
}

// The attributes of a RenderSettings prim that its products inherit, as
// UsdRender resolves them (with the schema's fallbacks): the implicit
// product of settings without products takes them.
struct SettingsBase {
  std::array<std::int32_t, 2> resolution{2048, 1080};
  float pixel_aspect_ratio = 1.0F;
  pxr::TfToken aspect_ratio_conform_policy;
  std::array<float, 4> data_window_ndc{0.0F, 0.0F, 1.0F, 1.0F};
  MotionBlur motion_blur = MotionBlur::kEnabled;
  DepthOfField depth_of_field = DepthOfField::kEnabled;
};

// The policy of a conformPolicy token: expandAperture, the fallback, for an
// unknown one.
[[nodiscard]] AspectRatioConformPolicy ConformPolicy(const pxr::TfToken& token);

// The dicing rate of subdivision surfaces, umbra:dicingRate, as a
// RenderSettings prim authors it.
struct DicingRate {
  // In pixels: 1 when it is not authored, or invalid.
  double pixels = 1.0;
  // Why it is invalid: it is not a float or double above 0. Empty when it
  // is valid.
  std::string invalid;
};

// A RenderSettings prim, synced: Hydra's flattened products, the attributes
// they inherit, and the dicing rate.
class RenderSettingsPrim final : public pxr::HdRenderSettings {
 public:
  // Counts its sync into `counter`, unless it is a fallback prim.
  RenderSettingsPrim(const pxr::SdfPath& prim_id,
                     SyncCounter* UMBRA_NULLABLE counter);

  [[nodiscard]] const SettingsBase& base() const UMBRA_LIFETIMEBOUND {
    return base_;
  }
  [[nodiscard]] const DicingRate& dicing_rate() const UMBRA_LIFETIMEBOUND {
    return dicing_rate_;
  }
  // The camera it names, an empty path for none.
  [[nodiscard]] pxr::SdfPath CameraPath() const;

 protected:
  // NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp): the
  // name of the method OpenUSD lets render settings override.
  void _Sync(pxr::HdSceneDelegate* scene_delegate,
             pxr::HdRenderParam* UMBRA_NULLABLE render_param,
             const pxr::HdDirtyBits* dirty_bits) override;
  // NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

 private:
  SyncCounter* UMBRA_NULLABLE counter_;
  SettingsBase base_;
  DicingRate dicing_rate_;
};

// The images of the products of `settings` that see through its camera,
// or the settings' own when none does.
[[nodiscard]] std::vector<device::Image> ImagesOf(
    const RenderSettingsPrim& settings);

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_RENDER_SETTINGS_PRIM_H_
