// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// What a render var of each source is, in one table that every part of the
// delegate reads: the name of its AOV, the source a device renders for it,
// its channels and whether the denoise reads it; and the image a render
// product renders.

#ifndef UMBRA_DELEGATE_RENDER_VARS_H_
#define UMBRA_DELEGATE_RENDER_VARS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "umbra/core/safety.h"
#include "umbra/delegate/render_settings.h"
#include "umbra/device/image.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::delegate::internal {

// How the vars of a source are given on the stage.
enum class RenderVarKind : std::uint8_t {
  // A raw var, of sourceType raw, whose sourceName is the source's name.
  kRaw,
  // An lpe or primvar var, named by its own sourceName.
  kNamed,
};

// Whether the denoise of a denoisedColor var reads the vars of a source.
enum class DenoiseRole : std::uint8_t {
  kNone,
  kInput,
};

// What a render var of one source is.
struct RenderVarTraits {
  RenderVarSource source = RenderVarSource::kColor;
  // The name of the AOV that binds its render buffer, which is also the
  // sourceName of its raw var, if it has one.
  std::string_view name;
  // How its vars are given on the stage.
  RenderVarKind kind = RenderVarKind::kRaw;
  // The source a device renders for it: none for those no device renders.
  std::optional<device::VarSource> rendered;
  // Its channels: 0 for a primvar var, which has as many as its dataType
  // has components.
  std::uint32_t channels = 0;
  // Whether the denoise of a denoisedColor var reads it.
  DenoiseRole denoise = DenoiseRole::kNone;
};

// Every source's, in the order of RenderVarSource.
inline constexpr std::array<RenderVarTraits, 7> kRenderVars = {
    {
        {
            .source = RenderVarSource::kColor,
            .name = "color",
            .kind = RenderVarKind::kRaw,
            .rendered = device::VarSource::kColor,
            .channels = 4,
            .denoise = DenoiseRole::kInput,
        },
        {
            .source = RenderVarSource::kDenoisedColor,
            .name = "denoisedColor",
            .kind = RenderVarKind::kRaw,
            .rendered = std::nullopt,
            .channels = 4,
            .denoise = DenoiseRole::kNone,
        },
        {
            .source = RenderVarSource::kDepth,
            .name = "depth",
            .kind = RenderVarKind::kRaw,
            .rendered = device::VarSource::kDepth,
            .channels = 1,
            .denoise = DenoiseRole::kNone,
        },
        {
            .source = RenderVarSource::kNormal,
            .name = "normal",
            .kind = RenderVarKind::kRaw,
            .rendered = device::VarSource::kNormal,
            .channels = 3,
            .denoise = DenoiseRole::kInput,
        },
        {
            .source = RenderVarSource::kAlbedo,
            .name = "albedo",
            .kind = RenderVarKind::kRaw,
            .rendered = device::VarSource::kAlbedo,
            .channels = 3,
            .denoise = DenoiseRole::kInput,
        },
        {
            .source = RenderVarSource::kLpe,
            .name = "lpe",
            .kind = RenderVarKind::kNamed,
            .rendered = std::nullopt,
            .channels = 3,
            .denoise = DenoiseRole::kNone,
        },
        {
            .source = RenderVarSource::kPrimvar,
            .name = "primvar",
            .kind = RenderVarKind::kNamed,
            .rendered = std::nullopt,
            .channels = 0,
            .denoise = DenoiseRole::kNone,
        },
    },
};

// Whether kRenderVars holds each source at its own index.
[[nodiscard]] constexpr bool IsInSourceOrder() {
  for (std::size_t index = 0; index < kRenderVars.size(); ++index) {
    if (static_cast<std::size_t>(kRenderVars.at(index).source) != index) {
      return false;
    }
  }
  return true;
}
static_assert(IsInSourceOrder());

// What a render var of `source` is.
[[nodiscard]] constexpr const RenderVarTraits& RenderVarOf(
    RenderVarSource source) {
  return kRenderVars.at(static_cast<std::size_t>(source));
}

// The image `product` renders: its resolution, its data window, its pixel
// aspect ratio and its conform policy.
[[nodiscard]] constexpr device::Image ImageOf(const RenderProduct& product) {
  return {
      .resolution = product.resolution,
      .window = product.window,
      .pixel_aspect_ratio = product.pixel_aspect_ratio,
      .aspect_ratio_conform_policy = product.aspect_ratio_conform_policy,
  };
}

}  // namespace umbra::delegate::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DELEGATE_RENDER_VARS_H_
