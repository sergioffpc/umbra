// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Motion samples (docs/adr/0012-umbra-is-a-hydra-renderer.md): how many a
// value sampled over the shutter gets, and its values at those equally
// spaced times, blended between the samples Hydra gives of it.

#ifndef UMBRA_EXT_USD_MOTION_SAMPLES_H_
#define UMBRA_EXT_USD_MOTION_SAMPLES_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

#include "host_copy.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::usd::internal {

// The most motion samples a prim gets.
inline constexpr std::size_t kMaxMotionSamples = 16;

// Whether `shutter` spans an interval: whether there is motion blur.
[[nodiscard]] bool IsMotionBlurred(const host::Shutter& shutter);

// What sampling did with the motion of a prim, for the warnings that report
// it, from the least to the most severe: a prim's outcome is the most severe
// of its values'.
enum class MotionOutcome : std::uint8_t {
  // Sampled as given: still, or with at most kMaxMotionSamples samples.
  kSampled,
  // It has more than kMaxMotionSamples motion samples over the shutter, and
  // was resampled down to that many.
  kResampledDown,
  // Its topology changes within the shutter: it has its sample at the frame
  // alone, without blur.
  kTopologyChanges,
};

// A value sampled at times relative to the frame, in ascending order, as
// Hydra gives it: the samples within the shutter and those that bracket it.
template <typename T>
struct TimedSamples {
  std::vector<float> times;
  std::vector<T> values;
};

// How many motion samples a value gets, and whether it was resampled down.
struct MotionSampleCount {
  std::size_t count = 1;
  MotionOutcome outcome = MotionOutcome::kSampled;
};

// How many motion samples a value sampled at `times` gets over `shutter`:
// the shutter's ends and as many as its times strictly within it, at most
// kMaxMotionSamples. With one time within a shutter centered on it, the
// middle sample is its value at that time. Its times at the ends count as the
// ends: Hydra gives the ends of the interval as sample times for some values,
// authored or not. One when there is no motion blur or a single sample.
[[nodiscard]] MotionSampleCount CountMotionSamples(
    std::span<const float> times, const host::Shutter& shutter);

// The count two values sampled together get: the larger, with the more
// severe outcome.
[[nodiscard]] MotionSampleCount Merge(const MotionSampleCount& lhs,
                                      const MotionSampleCount& rhs);

// The time of sample `index` of `count` equally spaced over `shutter`.
[[nodiscard]] double UniformTime(const host::Shutter& shutter,
                                 std::size_t index, std::size_t count);

// The value of `samples` at `time`, blended by `blend` between the two
// samples that bracket it; the first or the last sample outside them.
template <typename T, typename Blend>
T ValueAt(const TimedSamples<T>& samples, double time, const Blend& blend) {
  const auto after = std::ranges::upper_bound(
      samples.times, time, std::ranges::less{},
      [](float sample_time) { return static_cast<double>(sample_time); });
  const auto next =
      static_cast<std::size_t>(std::distance(samples.times.begin(), after));
  if (next == 0) {
    return samples.values.front();
  }
  if (next == samples.times.size()) {
    return samples.values.back();
  }
  const auto start = static_cast<double>(samples.times.at(next - 1));
  const auto end = static_cast<double>(samples.times.at(next));
  if (time <= start) {
    return samples.values.at(next - 1);
  }
  return blend(samples.values.at(next - 1), samples.values.at(next),
               (time - start) / (end - start));
}

// The values of `samples` at `count` equally spaced times over `shutter`.
template <typename T, typename Blend>
std::vector<T> Resample(const TimedSamples<T>& samples,
                        const host::Shutter& shutter, std::size_t count,
                        const Blend& blend) {
  std::vector<T> resampled;
  resampled.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    resampled.push_back(
        ValueAt(samples, UniformTime(shutter, index, count), blend));
  }
  return resampled;
}

// Whether every sample of `samples` is as long as the first: whether the
// topology they give is the same throughout.
template <typename T>
bool HasSameSizes(const std::vector<T>& samples) {
  return std::ranges::all_of(samples, [&](const T& sample) {
    return sample.size() == samples.front().size();
  });
}

// `samples` cut down to their first when they are all equal: a value that
// does not move has one sample.
template <typename T>
void CollapseStill(std::vector<T>& samples) {
  if (samples.size() > 1 && std::ranges::all_of(samples, [&](const T& sample) {
        return sample == samples.front();
      })) {
    samples.resize(1);
  }
}

// A value's motion samples, equally spaced over the shutter, and what
// sampling did.
template <typename T>
struct Motion {
  // None when there is no motion blur or the value does not move.
  std::vector<T> samples;
  MotionOutcome outcome = MotionOutcome::kSampled;
};

// The motion samples of a value sampled at `samples` over `shutter`,
// blended by `blend` between them.
template <typename T, typename Blend>
Motion<T> MotionOf(const TimedSamples<T>& samples, const host::Shutter& shutter,
                   const Blend& blend) {
  const MotionSampleCount count = CountMotionSamples(samples.times, shutter);
  if (count.count < 2) {
    return {};
  }
  std::vector<T> resampled = Resample(samples, shutter, count.count, blend);
  CollapseStill(resampled);
  if (resampled.size() == 1) {
    return {};
  }
  return {.samples = std::move(resampled), .outcome = count.outcome};
}

}  // namespace umbra::ext::usd::internal

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_USD_MOTION_SAMPLES_H_
