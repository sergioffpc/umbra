// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The bridge to OpenUSD, through which umbra renders as a Hydra renderer
// (docs/adr/0012-umbra-is-a-hydra-renderer.md): the prims of its render
// delegate convert what they sync into host copies and commit them to the
// devices (docs/adr/0013-devices-render-from-their-own-memory.md). Its
// diagnostics go to umbra's log as the usd component, and no OpenUSD type
// leaves it.

#ifndef UMBRA_EXT_USD_USD_H_
#define UMBRA_EXT_USD_USD_H_

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/device.h"
#include "umbra/ext/usd/render_settings.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd {

// Initializes OpenUSD for the process: routes its errors, warnings and status
// messages into umbra's log, sends its TF_DEBUG output to stderr, loads the
// plugin registry, and logs its version at info. Call it once at startup,
// after core::ConfigureLogging. Fails with kUnavailable when no plugin reads
// .usda files, which means OpenUSD's plugins were not found next to its
// libraries.
[[nodiscard]] std::expected<void, core::Error> Initialize();

// How a stage is loaded.
struct LoadOptions {
  // The path of the RenderSettings prim to render with, or empty for the
  // stage's renderSettingsPrimPath.
  std::string settings;
};

// The time codes a stage's animation spans, both included.
struct TimeCodes {
  double start = 0.0;
  double end = 0.0;
};

// What a render keeps alive while it renders: the stage, its render index
// and umbra's render delegate, whose prims stay synced from frame to frame,
// and the render settings and frame parameters of the frame committed last.
class Session {
 public:
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) = delete;
  Session& operator=(Session&&) = delete;
  ~Session();

  // The render settings and the frame parameters of the frame committed
  // last, once one is.
  [[nodiscard]] const RenderSettings& settings() const UMBRA_LIFETIMEBOUND;
  [[nodiscard]] const device::FrameParameters& frame() const
      UMBRA_LIFETIMEBOUND;

  // The stage's startTimeCode and endTimeCode: the one it authors for both
  // when it authors only one, and none when it authors neither.
  [[nodiscard]] std::optional<TimeCodes> time_codes() const;

  // Syncs the stage at `frame`, or at the default time for none, through
  // Hydra into the host copies of the delegate's prims, then commits them to
  // every device and frees them, as a phase of the render: "Syncing scene"
  // for the first frame, which adds every prim, and "Syncing frame <frame>"
  // for each later one, which gives the devices only the prims that
  // changed since the last, with what changed in each, and the prims
  // removed. The stage, its textures and what does not change are loaded
  // once. A frame commits the render settings, which stay on the host,
  // their camera and their products, whose files are checked writable,
  // their missing directories created. Settings without products
  // write <scene-stem>.<frame>.exr, in the current directory, with the color.
  // Render vars umbra does not render are left out, with one warning. The
  // cameras committed are those the settings and their products see through.
  // The visible meshes, implicit surfaces among them, become prototypes, held
  // once, with their instances: one for a mesh not instanced, and one for
  // each instance of native instancing and PointInstancers, nested
  // instancers flattened into one level, with the primvars they give per
  // instance. Motion is sampled within the shutter of the settings' camera,
  // none when the settings disable motion blur: the transforms of the
  // instances and cameras, and the points of the meshes, that move get
  // equally spaced samples over it, as many as they have within it, at least
  // 2 and at most 16. Every Material prim is committed, from its first sync
  // to its removal, as a neutral network of its surface, of MaterialX
  // nodedefs (UsdPreviewSurface's nodes as MaterialX's) and .oso shaders,
  // their colors converted into ACEScg through OpenColorIO from their color
  // space, and their textures resolved by Ar to absolute files, a UDIM set
  // keeping its <UDIM> token, each with its color space, and committed once
  // for every material and light that reads it. A material that does
  // not translate is rendered as an 18% gray diffuse; a mesh without one, as
  // its displayColor, converted into ACEScg, when it has one. The visible
  // UsdLux lights (sphere, rect, disk, cylinder, distant and dome lights)
  // are committed with their shapes in meters, placed by their instances, their
  // colors converted into ACEScg, their color temperatures kept as such, and
  // their textures resolved as the materials' are, those with no color space
  // sRGB when the file is and scene-linear otherwise; a DomeLight_1's
  // orientation follows its poleAxis, the stage's upAxis for "scene". Lights
  // are physical: their diffuse and specular multipliers, focus, cone softness
  // and shadow controls are not taken. One warning each reports the meshes
  // rendered as the cage of the subdivision they ask for, the invalid ones,
  // dropped, the curves, points and volumes, left out, the materials that do
  // not translate, the textures that do not resolve, rendered with their node's
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
  // A warning a frame logged is not logged again by a later one.
  //
  // Fails with kNotFound when no RenderSettings prim is at the settings'
  // path; kInvalidArgument when the settings or a product have no camera or
  // one that is not a Camera, a product has no orderedVars or its file is
  // not .exr, or a material reads a texture or shader, or a light a
  // texture, inside a package (.usdz); kUnavailable when a product's file
  // cannot be written; and with a device's error, naming the device, when
  // its commit fails. The session renders no further frame then.
  [[nodiscard]] std::expected<void, core::Error> SyncFrame(
      std::optional<double> frame);

  // Its state, with OpenUSD's types: the bridge's own.
  struct State;

  explicit Session(std::unique_ptr<State> state);

 private:
  std::unique_ptr<State> state_;
};

// Opens a session on the USD file at `path`, as the render's first phase,
// "Loading stage": opens its stage, with every payload loaded and no
// session layer; warnings while composing (a missing sublayer) go to the
// log. Its frames are committed to `devices`, which must outlive the
// session.
//
// Fails with kNotFound when the file does not exist, and kInvalidArgument
// when the file cannot be read or its stage does not open or compose, there
// are no settings, or the settings' path is not that of a prim.
[[nodiscard]] std::expected<std::unique_ptr<Session>, core::Error> OpenSession(
    std::string_view path, const LoadOptions& options,
    std::span<device::Device* const> devices UMBRA_LIFETIMEBOUND);

}  // namespace umbra::ext::usd

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_USD_H_
