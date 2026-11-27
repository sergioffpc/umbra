// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "frames.h"

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>

#include "umbra/core/error.h"

namespace umbra {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

// A time code: an integer or a decimal fraction ("12", "12.5", "-3"),
// without an exponent. Fails naming `what` and `text`.
std::expected<double, Error> ParseTimeCode(std::string_view text,
                                           std::string_view what) {
  // A digit leads, after the sign: ".5" and "1...3" are malformed.
  const std::string_view digits = text.starts_with('-') ? text.substr(1) : text;
  const bool leads_with_digit =
      !digits.empty() && digits.front() >= '0' && digits.front() <= '9';
  double value = 0.0;
  const char* const first = std::to_address(text.begin());
  const char* const last = std::to_address(text.end());
  const auto [end, error] =
      std::from_chars(first, last, value, std::chars_format::fixed);
  const bool is_whole = end == last;
  if (!leads_with_digit || error != std::errc() || !is_whole ||
      !std::isfinite(value)) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("invalid {} '{}': a time code such as 12 or 12.5 is "
                          "expected",
                          what, text)));
  }
  return value;
}

}  // namespace

std::expected<FrameRange, Error> ParseFrames(std::string_view text) {
  constexpr std::string_view kSeparator = "..";
  const std::size_t separator = text.find(kSeparator);
  const std::string_view first = text.substr(0, separator);
  const std::string_view last =
      separator == std::string_view::npos
          ? first
          : text.substr(separator + kSeparator.size());
  const auto start = ParseTimeCode(first, "first frame");
  if (!start.has_value()) {
    return std::unexpected(start.error());
  }
  const auto end = ParseTimeCode(last, "last frame");
  if (!end.has_value()) {
    return std::unexpected(end.error());
  }
  if (*start > *end) {
    return std::unexpected(Error(
        ErrorCode::kInvalidArgument,
        std::format("invalid frames '{}': the first frame follows the last",
                    text)));
  }
  return FrameRange{.start = *start, .end = *end};
}

std::expected<double, Error> ParseStep(std::string_view text) {
  const auto step = ParseTimeCode(text, "step");
  if (!step.has_value()) {
    return step;
  }
  if (*step <= 0.0) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("invalid step '{}': it must be above 0", text)));
  }
  return step;
}

std::expected<Frames, Error> Frames::Of(const FrameRange& range, double step) {
  // The most frames a range gives: its frame indices stay exact.
  constexpr double kMaxCount = 9007199254740992.0;  // 2^53
  // A step that does not divide the range exactly still reaches its end
  // when it misses it by rounding alone.
  constexpr double kTolerance = 1e-9;
  const double count =
      std::floor(((range.end - range.start) / step) + kTolerance) + 1.0;
  if (count > kMaxCount) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("{:.3g} frames, more than one process renders; "
                          "give fewer --frames or a larger --step",
                          count)));
  }
  return Frames(range, step, static_cast<std::uint64_t>(count));
}

std::optional<double> Frames::At(std::uint64_t index) const {
  if (!range_.has_value()) {
    return std::nullopt;
  }
  constexpr double kNanoframes = 1e9;
  const double frame = range_->start + (static_cast<double>(index) * step_);
  return std::round(frame * kNanoframes) / kNanoframes;
}

std::expected<Frames, Error> FramesOf(
    const std::optional<FrameRange>& requested,
    const std::optional<FrameRange>& stage, double step) {
  if (requested.has_value()) {
    return Frames::Of(*requested, step);
  }
  if (!stage.has_value()) {
    return Frames();
  }
  if (stage->start > stage->end) {
    return std::unexpected(
        Error(ErrorCode::kInvalidArgument,
              std::format("the stage's endTimeCode, {}, precedes its "
                          "startTimeCode, {}; give --frames",
                          stage->end, stage->start)));
  }
  return Frames::Of(*stage, step);
}

}  // namespace umbra
