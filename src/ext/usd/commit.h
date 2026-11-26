// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The commit, once Hydra has synced the prims into their host copies: the
// render settings the render uses, which stay on the host, and the change
// set every device receives, its cameras, materials, prototypes, lights and
// textures, all added
// (docs/adr/0013-devices-render-from-their-own-memory.md).

#ifndef UMBRA_EXT_USD_COMMIT_H_
#define UMBRA_EXT_USD_COMMIT_H_

#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>

#include <expected>
#include <optional>
#include <span>
#include <string>

#include "hydra.h"
#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/device.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd::internal {

// What a commit needs, besides the synced prims.
struct CommitInputs {
  // The RenderSettings prim the render uses.
  pxr::SdfPath settings;
  // The scene's file, which names the implicit product.
  std::string scene_path;
  // The frame, or none for the default time.
  std::optional<double> frame;
};

// Commits the host copies of the synced prims to `devices`, every prim
// added; `stage` tells which render vars author their dataType. Fails with
// kNotFound when no RenderSettings prim is at the settings' path, and
// kInvalidArgument when the settings or a product have no camera or one
// that is not a Camera, a product has no orderedVars or none umbra renders,
// or a product's file is not .exr; kUnavailable when a file cannot be
// written. Unsupported render vars are left out, with one warning. The
// cameras committed are those the settings and their products see through.
// Each mesh converted with an instance or more becomes a prototype with its
// instances and its materials, its own and those of its GeomSubsets: the
// one bound when it translates, else the fallback material; with none
// bound, the display color material when the mesh has a displayColor, and
// the fallback material otherwise. The materials committed are those of
// the prototypes, each once, and the lights those converted with an
// instance or more; the textures are those they read, each once. Fails with
// kInvalidArgument when one of the materials reads a texture or shader
// inside a package, or a light a texture. One warning each reports the
// materials that do not translate, the textures that do not resolve, the
// materials that connect each terminal but the surface, the meshes that ask
// for a subdivision scheme, the invalid ones, the rprims of each type not
// supported, the mesh lights, the lights of each type not supported, each
// issue of the lights (LightIssue), the lights of `scene_index`, the
// terminal scene index, that link geometry, the prims whose topology
// changes within the shutter and those resampled down to kMaxMotionSamples,
// with their count and the first. The frame's shutter is that the prims were
// sampled over. Then each device commits the change set, in order; the
// first that fails stops the commit, with its error, naming it.
[[nodiscard]] std::expected<Committed, core::Error> Commit(
    const CommitInputs& inputs, const pxr::UsdStage& stage,
    const pxr::HdSceneIndexBaseRefPtr& scene_index, SyncedPrims& prims,
    std::span<device::Device* const> devices);

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_COMMIT_H_
