// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/core/lazy_format.h"

#include <gtest/gtest.h>

#include <format>
#include <string>
#include <string_view>

namespace umbra::core {
namespace {

// A value that counts the times it is formatted into `formats`.
struct Counted {
  int* formats;
};

}  // namespace
}  // namespace umbra::core

// Formats a Counted as "counted", and counts it.
template <>
struct std::formatter<umbra::core::Counted> : std::formatter<std::string_view> {
  std::format_context::iterator format(const umbra::core::Counted& counted,
                                       std::format_context& context) const {
    ++*counted.formats;
    return std::formatter<std::string_view>::format("counted", context);
  }
};

namespace umbra::core {
namespace {

// The message `message` makes, as a call that fails formats it.
std::string Formatted(const LazyFormat& message) { return message.Format(); }

// Leaves `message` unformatted, as a call that succeeds does.
void Unformatted(const LazyFormat& /*message*/) {}

TEST(LazyFormatTest, FormatsItsArgumentsWhenAsked) {
  const std::string_view verb = "use";
  const int ordinal = 3;
  EXPECT_EQ(Formatted(LazyFormat("cannot {} device {}",
                                 std::make_format_args(verb, ordinal))),
            "cannot use device 3");
}

TEST(LazyFormatTest, FormatsAStringWithoutArguments) {
  EXPECT_EQ(
      Formatted(LazyFormat("cannot create a stream", std::make_format_args())),
      "cannot create a stream");
}

TEST(LazyFormatTest, FormatsNothingUntilAsked) {
  int formats = 0;
  const Counted counted{.formats = &formats};
  Unformatted(LazyFormat("{}", std::make_format_args(counted)));
  EXPECT_EQ(formats, 0);
  EXPECT_EQ(Formatted(LazyFormat("{}", std::make_format_args(counted))),
            "counted");
  EXPECT_EQ(formats, 1);
}

}  // namespace
}  // namespace umbra::core
