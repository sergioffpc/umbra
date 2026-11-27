// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The frames `umbra render` renders: the time codes of --frames and --step,
// or of the stage, one apart by default, or the default time alone.

#ifndef UMBRA_APPS_UMBRA_FRAMES_H_
#define UMBRA_APPS_UMBRA_FRAMES_H_

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/delegate/session.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra {

using FrameRange = delegate::TimeCodes;

// The frames of --frames: "<start>..<end>", both included, or "<frame>"
// alone, each a time code, an integer or a decimal fraction ("12", "12.5",
// "-3") without an exponent. Fails with kInvalidArgument when a time code is
// malformed or the start follows the end.
[[nodiscard]] std::expected<FrameRange, core::Error> ParseFrames(
    std::string_view text);

// The step of --step: a time code above 0. Fails with kInvalidArgument
// otherwise.
[[nodiscard]] std::expected<double, core::Error> ParseStep(
    std::string_view text);

// The frames a render renders: the time codes from a range's start to its
// end, both included, `step` apart, or the default time alone. Each is
// rounded to a nanoframe, so that 0.1 steps give 0.3, not
// 0.30000000000000004.
class Frames {
 public:
  // The default time alone.
  Frames() = default;

  // The frames of `range`, `step` apart. Fails with kInvalidArgument when
  // they are more than one process renders: more than 2^53, past which
  // their indices are no longer exact.
  [[nodiscard]] static std::expected<Frames, core::Error> Of(
      const FrameRange& range, double step);

  // The number of frames.
  [[nodiscard]] std::uint64_t count() const { return count_; }

  // The frame at `index`, below count(): none for the default time.
  [[nodiscard]] std::optional<double> At(std::uint64_t index) const;

 private:
  Frames(const FrameRange& range, double step, std::uint64_t count)
      : range_(range), step_(step), count_(count) {}

  std::optional<FrameRange> range_;
  double step_ = 1.0;
  std::uint64_t count_ = 1;
};

// The frames a render renders: those of `requested` (--frames), else the
// stage's start to end time codes, `stage`, else the default time alone,
// `step` apart. Fails with kInvalidArgument when the stage's end precedes
// its start, or the frames are more than Frames::Of allows.
[[nodiscard]] std::expected<Frames, core::Error> FramesOf(
    const std::optional<FrameRange>& requested,
    const std::optional<FrameRange>& stage, double step);

}  // namespace umbra

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_APPS_UMBRA_FRAMES_H_
