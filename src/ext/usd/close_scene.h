// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Closing the Scene, once Hydra has synced the prims: the render settings
// the render uses, their camera and their products, in umbra's types.

#ifndef UMBRA_EXT_USD_CLOSE_SCENE_H_
#define UMBRA_EXT_USD_CLOSE_SCENE_H_

#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include <expected>
#include <optional>
#include <string>

#include "hydra.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/scene/scene.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd::internal {

// What the Scene is closed with, besides the synced prims.
struct CloseInputs {
  // The RenderSettings prim the render uses.
  pxr::SdfPath settings;
  // The scene's file, which names the implicit product.
  std::string scene_path;
  // The frame, or none for the default time.
  std::optional<double> frame;
  // The stage's metersPerUnit.
  double meters_per_unit = 1.0;
};

// Closes the Scene from the synced prims; `stage` tells which render vars
// author their dataType. Fails with kNotFound when no RenderSettings prim is
// at the settings' path, and kInvalidArgument when the settings or a product
// have no camera or one that is not a Camera, a product has no orderedVars
// or none umbra renders, or a product's file is not .exr; kUnavailable when
// a file cannot be written. Unsupported render vars are left out, with one
// warning.
[[nodiscard]] std::expected<scene::Scene, core::Error> CloseScene(
    const CloseInputs& inputs, const pxr::UsdStage& stage,
    const SyncedPrims& prims);

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_CLOSE_SCENE_H_
