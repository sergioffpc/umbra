// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// IES profiles (IESNA LM-63), read through Ar and parsed by OpenUSD's
// pxrIES, which keeps their candelas photometric, as UsdLux asks, and their
// TILT data, which pxrIES does not read, parsed by umbra.

#ifndef UMBRA_EXT_USD_IES_PROFILE_H_
#define UMBRA_EXT_USD_IES_PROFILE_H_

#include <expected>
#include <string>

#include "host_copy.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd::internal {

// A profile read, with its TILT data or why it is not read.
struct ReadProfile {
  host::IesProfile profile;
  // Why its TILT data is not read: its TILT file does not resolve, cannot
  // be opened, or does not hold valid data, or its included data is not
  // valid. The profile is read without it. Empty when it is read.
  std::string tilt_not_read;
};

// The profile of the file that Ar resolved to `resolved`, in type C, its
// symmetries unfolded, unnormalized, with its TILT data: included, or in
// the file its TILT line names, which Ar resolves next to it. Fails with
// why when the file cannot be opened or is not a valid profile.
[[nodiscard]] std::expected<ReadProfile, std::string> ReadIesProfile(
    const std::string& resolved);

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_IES_PROFILE_H_
