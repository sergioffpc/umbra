// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Embree's errors as umbra's: a build that fails is kUnavailable, with
// Embree's message.

#ifndef UMBRA_EXT_EMBREE_ERRORS_H_
#define UMBRA_EXT_EMBREE_ERRORS_H_

#include <embree4/rtcore_device.h>

#include <expected>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::embree {

// The error of a build of `what` that failed with `code`.
[[nodiscard]] core::Error BuildError(RTCError code, std::string_view what);

// Embree's error on the calling thread, which reading clears, as the error
// of a build of `what`.
[[nodiscard]] core::Error BuildError(RTCDevice device, std::string_view what);

// Fails with Embree's error on the calling thread, if any, as the error of a
// build of `what`.
[[nodiscard]] std::expected<void, core::Error> CheckBuild(
    RTCDevice device, std::string_view what);

}  // namespace umbra::ext::embree

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_EMBREE_ERRORS_H_
