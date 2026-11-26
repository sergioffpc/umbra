// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Closing the Scene, once Hydra has synced the prims: the render settings
// the render uses, their camera and their products, the meshes and their
// materials, and the lights, in umbra's types.

#ifndef UMBRA_EXT_USD_CLOSE_SCENE_H_
#define UMBRA_EXT_USD_CLOSE_SCENE_H_

#include <pxr/imaging/hd/sceneIndex.h>
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
// warning. Each mesh converted with an instance or more becomes a prototype
// with its instances and its materials, its own and those of its
// GeomSubsets: the one bound when it translates, else the fallback
// material; with none bound, the display color material when the mesh has
// a displayColor, and the fallback material otherwise. The Scene holds the
// materials of its prototypes, each once, and the lights converted, one for
// each of their instances. Fails with kInvalidArgument when one of the
// materials reads a texture or shader inside a package, or a light a
// texture. One warning each reports the materials that do not translate,
// the textures that do not resolve, the materials that connect each
// terminal but the surface, the meshes that ask for a subdivision scheme,
// the invalid ones, the rprims of each type not supported, the mesh lights,
// the lights of each type not supported, each issue of the lights
// (LightIssue), the lights of `scene_index`, the terminal scene index, that
// link geometry, the prims whose topology changes within the shutter and
// those resampled down to kMaxMotionSamples, with their count and the
// first. The Scene's shutter is that the prims were sampled over.
[[nodiscard]] std::expected<scene::Scene, core::Error> CloseScene(
    const CloseInputs& inputs, const pxr::UsdStage& stage,
    const pxr::HdSceneIndexBaseRefPtr& scene_index, const SyncedPrims& prims);

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_CLOSE_SCENE_H_
