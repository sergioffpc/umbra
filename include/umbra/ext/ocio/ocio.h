// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The bridge to OpenColorIO, umbra's color management: its diagnostics go to
// umbra's log as the ocio component, and the process's current config is the
// one $OCIO names or, without it, OpenColorIO's built-in default config.
// ACEScg is the reference color space
// (docs/adr/0007-spectral-rendering-from-acescg.md).

#ifndef UMBRA_EXT_OCIO_OCIO_H_
#define UMBRA_EXT_OCIO_OCIO_H_

#include <expected>
#include <span>
#include <string>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::ocio {

// Initializes OpenColorIO for the process: routes its messages into umbra's
// log at the ocio component's threshold, loads and validates the config, and
// logs the versions at info. Call it once at startup, after
// core::ConfigureLogging. Fails with kUnavailable when the config cannot be
// loaded or is invalid.
[[nodiscard]] std::expected<void, core::Error> Initialize();

// The name in the current config of the color space named, or aliased,
// `name` ("srgb_texture" gives "sRGB Encoded Rec.709 (sRGB)" in the
// built-in config). Fails with kNotFound when the config has no such color
// space, and kUnavailable when OpenColorIO fails.
[[nodiscard]] std::expected<std::string, core::Error> ColorSpaceName(
    std::string_view name);

// Converts `rgb`, RGB triples one after another, in place, from the color
// space named, or aliased, `color_space` into ACEScg. Thread-safe. Fails
// with kNotFound when the config has no such color space or no ACEScg, and
// kInvalidArgument when `rgb` is not whole triples or the config cannot
// convert one into the other.
[[nodiscard]] std::expected<void, core::Error> ConvertToAcescg(
    std::string_view color_space, std::span<float> rgb);

}  // namespace umbra::ext::ocio

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OCIO_OCIO_H_
