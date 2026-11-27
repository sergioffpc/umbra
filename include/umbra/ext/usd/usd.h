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
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/device.h"
#include "umbra/ext/oidn/oidn.h"
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

// A render var of a product rendered, and its pixels: float32, `channels`
// per pixel of the product's data window, the pixels row by row from its top
// left.
struct RenderedVar {
  const RenderVar* var;
  std::span<const float> pixels;
};

// A render product rendered: its vars' pixels, in its order.
struct RenderedProduct {
  const RenderProduct* product;
  std::vector<RenderedVar> vars;
};

// What shows the products while they render, tile by tile. The render
// calls it one call at a time, from a host thread of its own while the
// devices render: Begin as a product starts, Update for each region of its
// data window whose pixels are final, as its tiles finish, Update for the
// whole window once a denoise has filled its denoisedColor vars, and End
// once it has rendered, failed or not.
// It never fails a render: a display that cannot show stops on its own.
class Display {
 public:
  Display() = default;
  Display(const Display&) = delete;
  Display& operator=(const Display&) = delete;
  Display(Display&&) = delete;
  Display& operator=(Display&&) = delete;
  virtual ~Display();

  // `product` starts rendering into `vars`, whose pixels stay valid until
  // End.
  virtual void Begin(const RenderProduct& product,
                     std::span<const RenderedVar> vars) = 0;
  // The pixels of `region`, within the product's data window, are final.
  virtual void Update(const device::PixelWindow& region) = 0;
  // The product has rendered, or failed.
  virtual void End() = 0;
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

  // The products of the frame rendered last, with their pixels, valid until
  // the next RenderFrame.
  [[nodiscard]] std::vector<RenderedProduct> Rendered() const
      UMBRA_LIFETIMEBOUND;

  // Renders the stage at `frame`, or at the default time for none: syncs it
  // and commits it to the devices, then renders each of its products on
  // every device, through Hydra.
  //
  // Hydra syncs the stage into the host copies of the delegate's prims, then
  // the delegate commits them to every device and frees them, as a phase of
  // the render: "Syncing scene"
  // for the first frame, which adds every prim, and "Syncing frame <frame>"
  // for each later one, which gives the devices only the prims that
  // changed since the last, with what changed in each, and the prims
  // removed. The stage, its textures and what does not change are loaded
  // once. A frame commits the render settings, which stay on the host,
  // their camera and their products, whose files are checked writable,
  // their missing directories created. Settings without products
  // write <scene-stem>.<frame>.exr, in the current directory, with the color.
  // The lpe and primvar render vars are read with their expression or
  // primvar's name, unchecked; the intrinsic ones and the raw ones umbra
  // does not know are left out, with one warning. The cameras committed are
  // those the settings and their products see through.
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
  // sRGB when the file is and scene-linear otherwise, and their IES profiles
  // read into candela tables, with their TILT data, included or in the file
  // their TILT line names, which gives each instance a tilt factor at its
  // inclination at the frame; a DomeLight_1's orientation follows its
  // poleAxis, the stage's upAxis for "scene". A mesh light's mesh is
  // committed with its emission, read as a light's. Lights and mesh lights
  // are physical: their diffuse and specular multipliers, focus, cone
  // softness, IES angle scale and shadow controls are not taken. One warning
  // each reports the meshes rendered as the cage of the subdivision they ask
  // for, the invalid ones, dropped, the curves, points and volumes, left out,
  // the materials that do not translate, the textures that do not resolve,
  // rendered with their node's fallback value, the materials that connect a
  // displacement or a volume, not rendered, the lights that link geometry
  // (every light lights everything), the geometry and plugin lights, left
  // out, the PortalLights no rendered dome light targets, left out, the dome
  // lights that target a prim that is not a PortalLight, the lights' filters,
  // ignored, their IES profiles not read, rendered without them, the TILT
  // data of their IES profiles not read, rendered without it, their color
  // temperatures not above 0 K, their colors that do not convert, rendered
  // white, their textures not read, each artistic control the lights and the
  // mesh lights author, the prims whose topology changes within the shutter,
  // rendered without motion blur, and those resampled down to 16 samples.
  //
  // A warning a frame logged is not logged again by a later one.
  //
  // Each product then renders through a render pass of its own, as a phase
  // of the render, "Rendering <file>", which counts the tiles of its data
  // window, the pixels whose centers lie in its dataWindowNDC: every device
  // renders on a host thread of its own, into render buffers of float32,
  // one per var, those of the lpe and primvar vars left at 0. Its samples
  // per pixel are umbra:samplesPerPixel, an int from the product, else from
  // the settings, else 64. A product with a denoisedColor var, a raw var
  // whose sourceName is "denoisedColor", renders its color, albedo and
  // normal, into render buffers of their own for those it has no var of,
  // which no file holds; once rendered, its color is denoised from them
  // through Open Image Denoise, on the whole image, into the denoisedColor
  // var, RGBA with the color's alpha, as a phase of its own,
  // "Denoising <file>", which counts its pixels. The denoiser is created
  // once, for the first product that has such a var, and serves every
  // later product and frame.
  //
  // Fails with kNotFound when no RenderSettings prim is at the settings'
  // path; kInvalidArgument when the settings or a product have no camera or
  // one that is not a Camera, a product has no orderedVars or its file is
  // not .exr, its dataWindowNDC holds no pixel or umbra:samplesPerPixel is
  // not an int above 0, or a material reads a texture or shader, or a light
  // a texture, inside a package (.usdz); kUnavailable when a product's file
  // cannot be written, or no device takes the tiles of a product; with a
  // device's error, naming the device, when its commit or its render fails;
  // and with Open Image Denoise's error, naming the product's file, when a
  // denoise fails.
  // The session renders no further frame then.
  [[nodiscard]] std::expected<void, core::Error> RenderFrame(
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
// session, its products' denoisedColor vars denoised on `denoise_device`,
// and its products shown on `display`, if any, which must outlive it too.
//
// Fails with kNotFound when the file does not exist, and kInvalidArgument
// when the file cannot be read or its stage does not open or compose, there
// are no settings, or the settings' path is not that of a prim.
[[nodiscard]] std::expected<std::unique_ptr<Session>, core::Error> OpenSession(
    std::string_view path, const LoadOptions& options,
    std::span<device::Device* const> devices UMBRA_LIFETIMEBOUND,
    const oidn::DenoiseDevice& denoise_device,
    Display* UMBRA_NULLABLE display UMBRA_LIFETIMEBOUND = nullptr);

}  // namespace umbra::ext::usd

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_USD_H_
