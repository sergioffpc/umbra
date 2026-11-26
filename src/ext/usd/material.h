// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Materials, as Hydra gives them, translated into their host copies, neutral
// material networks (docs/adr/0008-materials-cross-as-neutral-networks.md):
// nodes that are MaterialX nodedefs or .oso shaders, their parameters as
// umbra values, with colors in ACEScg
// (docs/adr/0007-spectral-rendering-from-acescg.md) and textures resolved to
// absolute files with their color spaces, and their connections.

#ifndef UMBRA_EXT_USD_MATERIAL_H_
#define UMBRA_EXT_USD_MATERIAL_H_

#include <pxr/base/tf/token.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialNetworkInterface.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "host_copy.h"
#include "umbra/core/error.h"
#include "umbra/core/progress.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd::internal {

namespace pxr = PXR_NS;

// The color space of a USD color that authors none: scene-linear Rec.709.
inline constexpr std::string_view kDefaultColorSpace = "lin_rec709_scene";

// The primvar a prim's display color is.
inline constexpr std::string_view kDisplayColor = "displayColor";

// The names of umbra's own materials, which no prim path can be.
inline constexpr std::string_view kFallbackMaterial = "fallback";
inline constexpr std::string_view kDisplayColorMaterial = "displayColor";

// A texture whose file does not resolve: its node renders its fallback.
struct UnresolvedTexture {
  // The path of its node's prim.
  pxr::SdfPath node;
  // The asset path, as authored.
  std::string asset;
};

// A material, translated: its network, or why it does not translate, the
// textures that do not resolve, and the terminals other than the surface
// that are connected (displacement, volume), which umbra does not take.
struct TranslatedMaterial {
  std::expected<host::MaterialNetwork, std::string> network;
  std::vector<UnresolvedTexture> unresolved;
  std::vector<std::string> unsupported_terminals;
};

// Translates the surface of the material network of `network`: each node's
// shader is a MaterialX nodedef (UsdPreviewSurface's nodes become theirs:
// ND_UsdPreviewSurface_surfaceshader, ND_UsdUVTexture, ...) or else the
// .oso shader its OSL:sourceAsset names; its colors, color arrays among
// them, are converted into ACEScg from their color space; its asset
// parameters are textures, resolved by Ar with their color space (auto
// becomes sRGB or raw, as the file tells), dropped when they do not
// resolve. The network does not translate when a node has an unknown shader
// or a parameter of a type umbra does not take, a color does not convert,
// or there is no surface. Fails with kInvalidArgument for a texture or
// shader inside a package (.usdz), which is not a file.
[[nodiscard]] std::expected<TranslatedMaterial, core::Error> TranslateMaterial(
    const pxr::HdMaterialNetworkInterface& network);

// A Material prim, synced: its network for MaterialX or the universal
// render context, translated.
class MaterialPrim final : public pxr::HdMaterial {
 public:
  // Counts its sync into `phase`, unless it is a fallback prim.
  MaterialPrim(const pxr::SdfPath& prim_id,
               core::ProgressPhase* UMBRA_NULLABLE phase);

  [[nodiscard]] pxr::HdDirtyBits GetInitialDirtyBitsMask() const override;
  void Sync(pxr::HdSceneDelegate* scene_delegate,
            pxr::HdRenderParam* UMBRA_NULLABLE render_param,
            pxr::HdDirtyBits* dirty_bits) override;

  // Its translation once synced, or the error that stops the render.
  [[nodiscard]] const std::expected<TranslatedMaterial, core::Error>&
  translation() const UMBRA_LIFETIMEBOUND {
    return translation_;
  }
  // Its network, its host copy, when it translates; none otherwise.
  [[nodiscard]] const host::MaterialNetwork* UMBRA_NULLABLE
  network() const UMBRA_LIFETIMEBOUND;
  // Frees its host copy, once committed: it is then as before its sync.
  void FreeHostCopy();

 private:
  core::ProgressPhase* UMBRA_NULLABLE phase_;
  std::expected<TranslatedMaterial, core::Error> translation_;
};

// The render contexts of the material networks umbra takes, the first
// authored chosen: MaterialX, then the universal one.
[[nodiscard]] const pxr::TfTokenVector& MaterialRenderContexts();

// The shader source types umbra takes: MaterialX, then OSL.
[[nodiscard]] const pxr::TfTokenVector& ShaderSourceTypes();

// The fallback material: an 18% gray diffuse.
[[nodiscard]] host::MaterialNetwork FallbackMaterial();

// The material of meshes without one that have a display color: a
// UsdPreviewSurface whose diffuseColor is their displayColor primvar, 18%
// gray where it has none.
[[nodiscard]] host::MaterialNetwork DisplayColorMaterial();

// The absolute file `asset` resolves to, through Ar, or none when it does
// not resolve. Fails with kInvalidArgument, naming `owner`, when it is
// inside a package (.usdz), which is not a file.
[[nodiscard]] std::expected<std::optional<std::string>, core::Error>
ResolveAsset(const pxr::SdfAssetPath& asset, const pxr::SdfPath& owner);

// Whether the texels of `file` are sRGB, by USD's convention: as its
// metadata says, or else when it is 8-bit with 3 or 4 channels. The first
// tile of a UDIM set tells.
[[nodiscard]] bool IsSrgb(const std::string& file);

// The color space of the texels of the texture `file`, by its name in the
// OpenColorIO config: `authored`, else `fallback`. auto becomes sRGB or
// raw, as the file tells. Fails with why when the config has no such color
// space.
[[nodiscard]] std::expected<std::string, std::string> TextureColorSpace(
    const pxr::TfToken& authored, const std::string& file,
    std::string_view fallback);

// Converts the displayColor of `primvars`, those of the prim at `prim_id`,
// if any, into ACEScg from the color space Hydra gives it,
// kDefaultColorSpace when none; drops it, with a warning, when it does not
// convert.
void ConvertDisplayColor(pxr::HdSceneDelegate& delegate,
                         const pxr::SdfPath& prim_id,
                         std::vector<host::Primvar>& primvars);

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_MATERIAL_H_
