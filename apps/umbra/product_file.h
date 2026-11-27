// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The OpenEXR file of a render product: one layer per render var, with the
// product's windows, camera matrices and frame.

#ifndef UMBRA_APPS_UMBRA_PRODUCT_FILE_H_
#define UMBRA_APPS_UMBRA_PRODUCT_FILE_H_

#include <expected>
#include <optional>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/session.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra {

// Writes `rendered`, a product of `frame`, as its OpenEXR file.
[[nodiscard]] std::expected<void, core::Error> WriteProduct(
    const delegate::RenderedProduct& rendered, std::optional<double> frame);

}  // namespace umbra

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_APPS_UMBRA_PRODUCT_FILE_H_
