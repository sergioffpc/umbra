// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "motion_samples.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "host_copy.h"

namespace umbra::delegate::internal {
namespace {

// A shutter centered on the frame.
constexpr host::Shutter kCentered{.open = -0.25, .close = 0.25};

TEST(CountMotionSamplesTest, KeepsTheTimeWithinTheShutterBetweenItsEnds) {
  // Values authored on whole frames: Hydra gives the frame and those that
  // bracket the shutter.
  constexpr std::array<float, 3> kTimes{-1.0F, 0.0F, 1.0F};

  const MotionSampleCount count = CountMotionSamples(kTimes, kCentered);

  EXPECT_EQ(count.count, 3U);
  EXPECT_EQ(count.outcome, MotionOutcome::kSampled);
}

TEST(CountMotionSamplesTest, GivesTheShuttersEndsWithNoTimeWithinIt) {
  // Hydra gives the shutter's ends as sample times for some values.
  constexpr std::array<float, 2> kTimes{-0.25F, 0.25F};

  EXPECT_EQ(CountMotionSamples(kTimes, kCentered).count, 2U);
}

TEST(CountMotionSamplesTest, GivesOneWithoutMotionBlur) {
  constexpr std::array<float, 3> kTimes{-1.0F, 0.0F, 1.0F};

  EXPECT_EQ(CountMotionSamples(kTimes, host::Shutter{}).count, 1U);
}

// The shutter from 0 to 1, and `within` times equally spaced strictly within
// it, bracketed by 0 and 1.
std::array<float, kMaxMotionSamples + 1> TimesWithin(std::size_t within) {
  std::array<float, kMaxMotionSamples + 1> times{};
  for (std::size_t index = 0; index < within + 2; ++index) {
    times.at(index) =
        static_cast<float>(index) / static_cast<float>(within + 1);
  }
  return times;
}

TEST(CountMotionSamplesTest, KeepsTheMostTimesWithinTheShutterThatFit) {
  constexpr std::size_t kWithin = kMaxMotionSamples - 2;
  const auto times = TimesWithin(kWithin);

  const MotionSampleCount count =
      CountMotionSamples(std::span(times).first(kWithin + 2),
                         host::Shutter{.open = 0.0, .close = 1.0});

  EXPECT_EQ(count.count, kMaxMotionSamples);
  EXPECT_EQ(count.outcome, MotionOutcome::kSampled);
}

TEST(CountMotionSamplesTest, ResamplesDownMoreTimesWithinTheShutter) {
  constexpr std::size_t kWithin = kMaxMotionSamples - 1;
  const auto times = TimesWithin(kWithin);

  const MotionSampleCount count =
      CountMotionSamples(times, host::Shutter{.open = 0.0, .close = 1.0});

  EXPECT_EQ(count.count, kMaxMotionSamples);
  EXPECT_EQ(count.outcome, MotionOutcome::kResampledDown);
}

// The value `weight` of the way from `start` to `end`.
double Lerp(double start, double end, double weight) {
  return (start * (1.0 - weight)) + (end * weight);
}

TEST(MotionOfTest, HasTheValueAtTheFrameAmongItsSamples) {
  // A value authored on whole frames that peaks at the frame.
  const TimedSamples<double> samples{
      .times = {-1.0F, 0.0F, 1.0F},
      .values = {0.0, 1.0, 0.0},
  };

  const Motion<double> motion = MotionOf(samples, kCentered, Lerp);

  EXPECT_EQ(motion.samples, (std::vector<double>{0.75, 1.0, 0.75}));
}

TEST(MotionOfTest, HasNoSamplesForAStillValue) {
  const TimedSamples<double> samples{
      .times = {-1.0F, 0.0F, 1.0F},
      .values = {2.0, 2.0, 2.0},
  };

  EXPECT_TRUE(MotionOf(samples, kCentered, Lerp).samples.empty());
}

}  // namespace
}  // namespace umbra::delegate::internal
