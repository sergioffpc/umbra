// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Opening a stage, with OpenUSD's errors and exceptions as std::expected. It
// gives OpenUSD's own handle to the render delegate, the module that reads
// the scene through OpenUSD's types
// (docs/adr/0017-ext-is-a-thin-layer-over-each-dependency.md).

#ifndef UMBRA_EXT_USD_STAGE_H_
#define UMBRA_EXT_USD_STAGE_H_

#include <pxr/pxr.h>
#include <pxr/usd/usd/common.h>

#include <expected>
#include <string>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd {

// Opens the stage of the file at `path`, with every payload loaded and no
// session layer. The errors of a stage that does not open become the
// returned error rather than log lines; those of a stage that opens are
// logged.
//
// Fails with kNotFound when the file does not exist, and kInvalidArgument
// when the file cannot be read or its stage does not open or compose.
[[nodiscard]] std::expected<PXR_NS::UsdStageRefPtr, core::Error> OpenStage(
    const std::string& path);

}  // namespace umbra::ext::usd

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_STAGE_H_
