// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// IES profiles (IESNA LM-63), read through Ar and parsed by OpenUSD's
// pxrIES, which keeps their candelas photometric, as UsdLux asks.

#ifndef UMBRA_EXT_USD_IES_PROFILE_H_
#define UMBRA_EXT_USD_IES_PROFILE_H_

#include <expected>
#include <string>

#include "host_copy.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd::internal {

// The profile of the file that Ar resolved to `resolved`, in type C, its
// symmetries unfolded, unnormalized. Fails with why when the file cannot be
// opened or is not a valid profile.
[[nodiscard]] std::expected<host::IesProfile, std::string> ReadIesProfile(
    const std::string& resolved);

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_IES_PROFILE_H_
