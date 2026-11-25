// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The files of the render products: their names, with the frame expanded,
// and the check that umbra can write them before the render starts.

#ifndef UMBRA_EXT_USD_OUTPUTS_H_
#define UMBRA_EXT_USD_OUTPUTS_H_

#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd::internal {

// `product_name` with $FF, $F4 and $F expanded for `frame`, none for the
// default time: $FF to the frame as given ("12", "12.5"), $F to its whole
// part ("12") and $F4 to that part zero-padded to 4 digits ("0012"). At the
// default time they expand to frame 0.
[[nodiscard]] std::string ExpandFrame(std::string_view product_name,
                                      std::optional<double> frame);

// The file of the implicit product of settings without products:
// "<scene-stem>.<frame>.exr", the frame's whole part zero-padded to 4 digits
// and its fraction kept ("scene.0012.exr", "scene.0012.5.exr"), or
// "<scene-stem>.exr" at the default time.
[[nodiscard]] std::string ImplicitProductFile(std::string_view scene_path,
                                              std::optional<double> frame);

// Checks that umbra can write `file`, an EXR file: its extension is .exr,
// its missing directories are created, and it can be created or
// overwritten. Fails with kInvalidArgument for another extension, and
// kUnavailable when it cannot be written.
[[nodiscard]] std::expected<void, core::Error> PrepareOutput(
    std::string_view file);

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_OUTPUTS_H_
