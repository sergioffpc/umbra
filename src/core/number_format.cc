// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/core/number_format.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace umbra::core {
namespace {

// The largest value shown with a prefix before the next one: 999.95 would
// round to "1000.0".
constexpr double kDecimalLimit = 999.95;
constexpr double kBinaryLimit = 1023.95;
constexpr double kThousand = 1000.0;
constexpr double kKibi = 1024.0;

constexpr std::array<std::string_view, 7> kSiPrefixes = {
    "", "k", "M", "G", "T", "P", "E",
};
constexpr std::array<std::string_view, 7> kBinaryUnits = {
    "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB",
};

// `value` divided by `base` until it is at most `limit`, or the prefixes run
// out, and the index of its prefix.
struct Scaled {
  double value = 0.0;
  std::size_t prefix = 0;
};

Scaled Scale(double value, double base, double limit, std::size_t prefixes) {
  Scaled scaled{.value = value, .prefix = 0};
  while (scaled.value >= limit && scaled.prefix + 1 < prefixes) {
    scaled.value /= base;
    ++scaled.prefix;
  }
  return scaled;
}

}  // namespace

std::string FormatDuration(std::chrono::duration<double> time) {
  constexpr std::int64_t kTenthsPerMinute = 600;
  constexpr std::int64_t kSecondsPerMinute = 60;
  constexpr std::int64_t kSecondsPerHour = 3600;
  constexpr std::int64_t kTenthsPerSecond = 10;
  const double seconds = std::max(time.count(), 0.0);
  const std::int64_t tenths =
      std::llround(seconds * static_cast<double>(kTenthsPerSecond));
  if (tenths < kTenthsPerMinute) {
    return std::format("{}.{} s", tenths / kTenthsPerSecond,
                       tenths % kTenthsPerSecond);
  }
  const std::int64_t whole = std::llround(seconds);
  if (whole < kSecondsPerHour) {
    return std::format("{}:{:02}", whole / kSecondsPerMinute,
                       whole % kSecondsPerMinute);
  }
  return std::format("{}:{:02}:{:02}", whole / kSecondsPerHour,
                     whole % kSecondsPerHour / kSecondsPerMinute,
                     whole % kSecondsPerMinute);
}

std::string FormatRate(double per_second, std::string_view unit) {
  const Scaled scaled = Scale(std::max(per_second, 0.0), kThousand,
                              kDecimalLimit, kSiPrefixes.size());
  return std::format("{:.1f} {}{}/s", scaled.value,
                     kSiPrefixes.at(scaled.prefix), unit);
}

std::string FormatBytes(std::uint64_t bytes) {
  if (static_cast<double>(bytes) < kKibi) {
    return std::format("{} B", bytes);
  }
  const Scaled scaled = Scale(static_cast<double>(bytes), kKibi, kBinaryLimit,
                              kBinaryUnits.size());
  return std::format("{:.1f} {}", scaled.value, kBinaryUnits.at(scaled.prefix));
}

std::string FormatCount(std::uint64_t count) {
  constexpr std::uint64_t kShortenAbove = 10'000;
  // The largest values shown with two and one decimals: 9.995 would round to
  // "10.00", 99.95 to "100.0", 999.5 to "1000".
  constexpr double kTwoDecimals = 9.995;
  constexpr double kOneDecimal = 99.95;
  constexpr double kNoDecimal = 999.5;
  if (count <= kShortenAbove) {
    return std::format("{}", count);
  }
  const Scaled scaled = Scale(static_cast<double>(count), kThousand, kNoDecimal,
                              kSiPrefixes.size());
  const std::string_view prefix = kSiPrefixes.at(scaled.prefix);
  if (scaled.value < kTwoDecimals) {
    return std::format("{:.2f}{}", scaled.value, prefix);
  }
  if (scaled.value < kOneDecimal) {
    return std::format("{:.1f}{}", scaled.value, prefix);
  }
  return std::format("{:.0f}{}", scaled.value, prefix);
}

}  // namespace umbra::core
