// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The numbers umbra shows on stderr, in its progress lines and its report
// (docs/adr/0002-cli-diagnostics-on-stderr.md):
//
//   38.2 s   1:05   1:02:03          durations
//   3.1 Gsamples/s   850.0 rays/s    rates, SI prefixes
//   6.2 GiB   512.0 MiB   900 B      sizes, binary prefixes
//   9876   12.3k   1.24M             counts, shortened above 10 000
//
// They always use a decimal point, whatever the locale.

#ifndef UMBRA_CORE_NUMBER_FORMAT_H_
#define UMBRA_CORE_NUMBER_FORMAT_H_

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::core {

// A duration: seconds with one decimal under a minute ("38.2 s"), then
// "m:ss", then "h:mm:ss". A negative duration is shown as zero.
[[nodiscard]] std::string FormatDuration(std::chrono::duration<double> time);

// A rate of `unit` per second, with an SI prefix and one decimal
// ("3.1 Gsamples/s", "850.0 samples/s").
[[nodiscard]] std::string FormatRate(double per_second, std::string_view unit);

// A size in bytes: "900 B" below a KiB, then a binary prefix and one decimal
// ("512.0 MiB", "6.2 GiB").
[[nodiscard]] std::string FormatBytes(std::uint64_t bytes);

// A count: in full up to 10 000 ("9876"), then three significant digits and
// k, M, G, T, P or E ("12.3k", "1.24M").
[[nodiscard]] std::string FormatCount(std::uint64_t count);

}  // namespace umbra::core

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_CORE_NUMBER_FORMAT_H_
