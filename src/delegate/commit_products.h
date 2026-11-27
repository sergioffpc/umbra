// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The render products of the render settings a commit commits, as the
// render writes them.

#ifndef UMBRA_DELEGATE_COMMIT_PRODUCTS_H_
#define UMBRA_DELEGATE_COMMIT_PRODUCTS_H_

#include <pxr/imaging/hd/renderSettings.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <vector>

#include "commit.h"
#include "commit_cameras.h"
#include "commit_warnings.h"
#include "hydra.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

// Converts the products of the render settings, as the render writes them,
// and tallies their vars that umbra does not render.
class ProductConverter {
 public:
  ProductConverter(const CommitInputs& inputs UMBRA_LIFETIMEBOUND,
                   const pxr::UsdStage& stage UMBRA_LIFETIMEBOUND,
                   Cameras& cameras UMBRA_LIFETIMEBOUND)
      : inputs_(&inputs), stage_(&stage), cameras_(&cameras) {}

  // The products of `settings`, or their implicit one, of one color var,
  // seen through `camera`, the settings' camera: each with its file
  // prepared, its framing, its vars, read from the stage, its samples per
  // pixel, and its camera's transforms at the frame, its camera given an id
  // by the cameras. The vars umbra does not render are left out, and the lpe
  // and primvar vars rendered as 0. Fails with kInvalidArgument when a
  // product has no orderedVars, no camera or one that is not a Camera, no
  // var umbra renders, a data window that holds no pixel, a file that is not
  // .exr, or an umbra:samplesPerPixel, its own or the settings', that is not
  // an int above 0; kUnavailable when a file cannot be written.
  [[nodiscard]] std::expected<std::vector<RenderProduct>, core::Error> Products(
      const RenderSettingsPrim& settings, device::CameraId camera);

  // "warning: usd: 3 render vars not supported (primId, first in
  // /Render/beauty); left out", and one of the vars rendered as 0.
  void Report(Warnings& warnings) const;

 private:
  // The product `product` converts into.
  [[nodiscard]] std::expected<RenderProduct, core::Error> Convert(
      const pxr::HdRenderSettings::RenderProduct& product);
  // The product of settings without products: one color var.
  [[nodiscard]] std::expected<RenderProduct, core::Error> Implicit(
      device::CameraId camera, const pxr::SdfPath& camera_path,
      const SettingsBase& base);
  // umbra:samplesPerPixel, from the product at `product`, if any, then from
  // the settings, else its default.
  [[nodiscard]] std::expected<std::uint32_t, core::Error> SamplesPerPixel(
      std::optional<pxr::SdfPath> product) const;

  const CommitInputs* inputs_;
  const pxr::UsdStage* stage_;
  Cameras* cameras_;
  // The vars left out, and those rendered as 0.
  Tally unsupported_;
  Tally zero_;
};

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_COMMIT_PRODUCTS_H_
