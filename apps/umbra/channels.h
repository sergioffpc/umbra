// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The names of the channels of a render var's layer, in the OpenEXR files
// and on a display.

#ifndef UMBRA_APPS_UMBRA_CHANNELS_H_
#define UMBRA_APPS_UMBRA_CHANNELS_H_

#include <cstdint>
#include <span>
#include <string_view>

#include "umbra/core/safety.h"
#include "umbra/delegate/render_settings.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra {

// The names of the channels of a layer of `source` with `channels`
// channels: R, G, B, A for colors, Z for depth, X, Y, Z, W for vectors.
[[nodiscard]] std::span<const std::string_view> ChannelNames(
    delegate::RenderVarSource source, std::uint32_t channels);

}  // namespace umbra

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_APPS_UMBRA_CHANNELS_H_
