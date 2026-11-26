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
// Render vars umbra does not render are left out, with one warning. The
// visible meshes, implicit surfaces among them, become prototypes, held
// once, with their instances: one for a mesh not instanced, and one for
// each instance of native instancing and PointInstancers, nested
// instancers flattened into one level, with the primvars they give per
// instance. Motion is sampled within the shutter of the settings' camera,
// none when the settings disable motion blur: the transforms of the
// instances and cameras, and the points of the meshes, that move get
// equally spaced samples over it, as many as they have within it, at least
// 2 and at most 16. Materials, those of the meshes and of their
// GeomSubsets, cross as neutral networks of their surface, of MaterialX
// nodedefs (UsdPreviewSurface's nodes as MaterialX's) and .oso shaders,
// their colors converted into ACEScg through OpenColorIO from their color
// space, and their textures resolved by Ar to absolute files, a UDIM set
// keeping its <UDIM> token, each with its color space. A material that does
// not translate is rendered as an 18% gray diffuse; a mesh without one, as
// its displayColor, converted into ACEScg, when it has one. The visible
// UsdLux lights (sphere, rect, disk, cylinder, distant and dome lights)
// cross with their shapes in meters, one for each instance, their colors
// converted into ACEScg, their color temperatures kept as such, and their
// textures resolved as the materials' are; a DomeLight_1's orientation
// follows its poleAxis, the stage's upAxis for "scene". Lights are physical:
// their diffuse and specular multipliers, focus, cone softness and shadow
// controls are not taken. One warning each reports the meshes rendered as
// the cage of the subdivision they ask for, the invalid ones, dropped, the
// curves, points and volumes, left out, the materials that do not
// translate, the textures that do not resolve, rendered with their node's
// fallback value, the materials that connect a displacement or a volume,
// not rendered, the lights that link geometry (every light lights
// everything), the portal, geometry, plugin and mesh lights, left out (a
// mesh light's mesh is rendered without emission), the lights' filters and
// IES profiles, ignored, their color temperatures not above 0 K, their
// colors that do not convert, rendered white, their textures not read, each
// artistic control the lights author, the prims whose topology changes
// within the shutter, rendered without motion blur, and those resampled
// down to 16 samples.
//
// Fails with kNotFound when the file does not exist or no RenderSettings
// prim is at the settings' path; kInvalidArgument when the file cannot be
// read or its stage does not open or compose, there are no settings, the
// settings or a product have no camera or one that is not a Camera, a
// product has no orderedVars or its file is not .exr, or a material reads a
// texture or shader, or a light a texture, inside a package (.usdz); and
// kUnavailable when a product's file cannot be written.
[[nodiscard]] std::expected<scene::Scene, core::Error> LoadScene(
    std::string_view path, const LoadOptions& options);

}  // namespace umbra::ext::usd

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_USD_H_
