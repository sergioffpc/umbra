// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "motion_samples.h"

#include <algorithm>
#include <cstddef>
#include <span>

#include "host_copy.h"

namespace umbra::ext::usd::internal {

bool IsMotionBlurred(const host::Shutter& shutter) {
  return shutter.open < shutter.close;
}

MotionSampleCount CountMotionSamples(std::span<const float> times,
                                     const host::Shutter& shutter) {
  if (!IsMotionBlurred(shutter) || times.size() < 2) {
    return {};
  }
  const auto within =
      static_cast<std::size_t>(std::ranges::count_if(times, [&](float time) {
        // Hydra's times are floats: its ends are those of the shutter, in
        // float.
        return time > static_cast<float>(shutter.open) &&
               time < static_cast<float>(shutter.close);
      }));
  // The shutter's ends, and the times between them.
  const std::size_t count = within + 2;
  if (count > kMaxMotionSamples) {
    return {
        .count = kMaxMotionSamples,
        .outcome = MotionOutcome::kResampledDown,
    };
  }
  return {.count = count, .outcome = MotionOutcome::kSampled};
}

MotionSampleCount Merge(const MotionSampleCount& lhs,
                        const MotionSampleCount& rhs) {
  return {
      .count = std::max(lhs.count, rhs.count),
      .outcome = std::max(lhs.outcome, rhs.outcome),
  };
}

double UniformTime(const host::Shutter& shutter, std::size_t index,
                   std::size_t count) {
  if (count < 2) {
    return 0.0;
  }
  const double fraction =
      static_cast<double>(index) / static_cast<double>(count - 1);
  return shutter.open + ((shutter.close - shutter.open) * fraction);
}

}  // namespace umbra::ext::usd::internal
