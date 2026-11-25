// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The bridge to OpenUSD, which reads the scenes umbra renders into umbra's
// Scene (docs/adr/0006-scenes-enter-through-hydra.md): its diagnostics go to
// umbra's log as the usd component, and no OpenUSD type leaves it.

#ifndef UMBRA_EXT_USD_USD_H_
#define UMBRA_EXT_USD_USD_H_

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/scene/scene.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd {

// Initializes OpenUSD for the process: routes its errors, warnings and status
// messages into umbra's log, sends its TF_DEBUG output to stderr, loads the
// plugin registry, and logs its version at info. Call it once at startup,
// after core::ConfigureLogging. Fails with kUnavailable when no plugin reads
// .usda files, which means OpenUSD's plugins were not found next to its
// libraries.
[[nodiscard]] std::expected<void, core::Error> Initialize();

// How a scene is loaded.
struct LoadOptions {
  // The time code to load, or none for the stage's startTimeCode when it
  // authors one, and the default time otherwise.
  std::optional<double> frame;
  // The path of the RenderSettings prim to render with, or empty for the
  // stage's renderSettingsPrimPath.
  std::string settings;
};

// Loads the scene of the USD file at `path`, in the render's first two
// phases. "Loading stage" opens its stage, with every payload loaded and no
// session layer; warnings while composing (a missing sublayer) go to the
// log. "Syncing scene" syncs the stage through Hydra into the Scene: the
// render settings, their camera and their products, whose files are checked
// writable, their missing directories created. Settings without products
// write <scene-stem>.<frame>.exr, in the current directory, with the color.
// Render vars umbra does not render are left out, with one warning.
//
// Fails with kNotFound when the file does not exist or no RenderSettings
// prim is at the settings' path; kInvalidArgument when the file cannot be
// read or its stage does not open or compose, there are no settings, the
// settings or a product have no camera or one that is not a Camera, a
// product has no orderedVars or its file is not .exr; and kUnavailable when
// a product's file cannot be written.
[[nodiscard]] std::expected<scene::Scene, core::Error> LoadScene(
    std::string_view path, const LoadOptions& options);

}  // namespace umbra::ext::usd

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_USD_H_
