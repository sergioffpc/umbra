// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "ies_tilt.h"

#include <gtest/gtest.h>

#include <format>
#include <string>
#include <string_view>

#include "umbra/device/render_data.h"

namespace umbra::delegate::internal {
namespace {

// A lamp whose output falls from 1 when hanging down to 0.6 when level and
// 0.2 when pointing up.
const IesTilt kTilt{
    .geometry = 1,
    .angles = {0.0F, 90.0F, 180.0F},
    .factors = {1.0F, 0.6F, 0.2F},
};

TEST(IesTiltFactorTest, IsTheFactorOfATiltAngle) {
  EXPECT_FLOAT_EQ(IesTiltFactor(kTilt, 0.0), 1.0F);
  EXPECT_FLOAT_EQ(IesTiltFactor(kTilt, 90.0), 0.6F);
  EXPECT_FLOAT_EQ(IesTiltFactor(kTilt, 180.0), 0.2F);
}

TEST(IesTiltFactorTest, InterpolatesLinearlyBetweenTiltAngles) {
  EXPECT_FLOAT_EQ(IesTiltFactor(kTilt, 45.0), 0.8F);
  EXPECT_FLOAT_EQ(IesTiltFactor(kTilt, 135.0), 0.4F);
  EXPECT_FLOAT_EQ(IesTiltFactor(kTilt, 22.5), 0.9F);
}

TEST(IesTiltFactorTest, HoldsTheFactorOfTheFirstOrLastAngleBeyondThem) {
  const IesTilt tilt{
      .geometry = 1,
      .angles = {30.0F, 60.0F},
      .factors = {0.5F, 0.7F},
  };
  EXPECT_FLOAT_EQ(IesTiltFactor(tilt, 0.0), 0.5F);
  EXPECT_FLOAT_EQ(IesTiltFactor(tilt, 180.0), 0.7F);
}

TEST(IesTiltFactorTest, IsConstantForASingleAngle) {
  const IesTilt tilt{.geometry = 2, .angles = {45.0F}, .factors = {0.8F}};
  EXPECT_FLOAT_EQ(IesTiltFactor(tilt, 0.0), 0.8F);
  EXPECT_FLOAT_EQ(IesTiltFactor(tilt, 120.0), 0.8F);
}

// Row-major, for row vectors.
constexpr device::Matrix kIdentity = {
    1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
};

// A rotation by 90 degrees about X: -Z turns to +Y.
constexpr device::Matrix kRotateXBy90 = {
    1.0, 0.0,  0.0, 0.0, 0.0, 0.0, 1.0, 0.0,
    0.0, -1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0,
};

// A rotation by -90 degrees about X: -Z turns to -Y. Scaled by 2 and
// translated, which change nothing.
constexpr device::Matrix kRotateXByMinus90 = {
    2.0, 0.0, 0.0, 0.0, 0.0, 0.0, -2.0, 0.0,
    0.0, 2.0, 0.0, 0.0, 5.0, 6.0, 7.0,  1.0,
};

// A rotation by 60 degrees about Y: -Z turns to (-sin 60, 0, -cos 60).
constexpr double kCos60 = 0.5;
constexpr double kSin60 = 0.8660254037844386;
constexpr device::Matrix kRotateYBy60 = {
    kCos60, 0.0, -kSin60, 0.0, 0.0, 0.0, 1.0, 0.0,
    kSin60, 0.0, kCos60,  0.0, 0.0, 0.0, 0.0, 1.0,
};

TEST(IesInclinationTest, IsZeroForALightFacingDownAZUpStage) {
  EXPECT_NEAR(IesInclination(kIdentity, UpAxis::kZ), 0.0, 1e-9);
}

TEST(IesInclinationTest, IsLevelForAnUnrotatedLightOnAYUpStage) {
  EXPECT_NEAR(IesInclination(kIdentity, UpAxis::kY), 90.0, 1e-9);
}

TEST(IesInclinationTest, FollowsTheLightsRotation) {
  EXPECT_NEAR(IesInclination(kRotateXBy90, UpAxis::kY), 180.0, 1e-9);
  EXPECT_NEAR(IesInclination(kRotateXByMinus90, UpAxis::kY), 0.0, 1e-9);
  EXPECT_NEAR(IesInclination(kRotateYBy60, UpAxis::kZ), 60.0, 1e-9);
  EXPECT_NEAR(IesInclination(kRotateYBy60, UpAxis::kY), 90.0, 1e-9);
}

TEST(IesInclinationTest, IsZeroWhenTheNadirHasNoLength) {
  device::Matrix flat = kIdentity;
  flat.at(10) = 0.0;
  EXPECT_EQ(IesInclination(flat, UpAxis::kY), 0.0);
}

// The header of a profile, before its TILT line.
constexpr std::string_view kHeader =
    "IESNA:LM-63-2002\r\n[TEST] tilt\r\n[MANUFAC] umbra\r\n";

// What follows the TILT data of a profile: its photometry.
constexpr std::string_view kPhotometry =
    "1 1000 1 2 1 1 2 0 0 0\n1 1 100\n0 90\n0\n100 50\n";

TEST(ParseIesTiltLineTest, ReadsNone) {
  const std::string profile =
      std::format("{}TILT=NONE\r\n{}", kHeader, kPhotometry);
  const auto line = ParseIesTiltLine(profile);
  ASSERT_TRUE(line.has_value()) << line.error();
  EXPECT_EQ(*line, IesTiltLine(IesTiltNone{}));
}

TEST(ParseIesTiltLineTest, ReadsIncludedData) {
  const std::string profile = std::format(
      "{}TILT=INCLUDE\r\n1\r\n3\r\n0, 90 ,180\r\n1.0 0.6\r\n0.2\r\n{}", kHeader,
      kPhotometry);
  const auto line = ParseIesTiltLine(profile);
  ASSERT_TRUE(line.has_value()) << line.error();
  EXPECT_EQ(*line, IesTiltLine(kTilt));
}

TEST(ParseIesTiltLineTest, ReadsTheNameOfATiltFile) {
  const std::string profile =
      std::format("{}TILT= lamp tilt.dat \r\n{}", kHeader, kPhotometry);
  const auto line = ParseIesTiltLine(profile);
  ASSERT_TRUE(line.has_value()) << line.error();
  EXPECT_EQ(*line, IesTiltLine(IesTiltFile{.name = "lamp tilt.dat"}));
}

TEST(ParseIesTiltLineTest, FailsWithoutATiltLine) {
  EXPECT_FALSE(ParseIesTiltLine(kHeader).has_value());
}

TEST(ParseIesTiltLineTest, FailsOnAGeometryOutsideOneToThree) {
  const std::string profile =
      std::format("{}TILT=INCLUDE\n4\n2\n0 90\n1 1\n{}", kHeader, kPhotometry);
  const auto line = ParseIesTiltLine(profile);
  ASSERT_FALSE(line.has_value());
  EXPECT_TRUE(line.error().contains("geometry")) << line.error();
}

TEST(ParseIesTiltLineTest, FailsOnAnglesThatDoNotAscendWithinZeroAnd180) {
  for (const std::string_view angles : {"90 0", "0 0", "0 190", "-5 90"}) {
    const std::string profile = std::format("{}TILT=INCLUDE\n1\n2\n{}\n1 1\n{}",
                                            kHeader, angles, kPhotometry);
    const auto line = ParseIesTiltLine(profile);
    ASSERT_FALSE(line.has_value()) << angles;
    EXPECT_TRUE(line.error().contains("angles")) << line.error();
  }
}

TEST(ParseIesTiltLineTest, FailsOnANegativeFactor) {
  const std::string profile = std::format(
      "{}TILT=INCLUDE\n1\n2\n0 90\n1 -0.5\n{}", kHeader, kPhotometry);
  const auto line = ParseIesTiltLine(profile);
  ASSERT_FALSE(line.has_value());
  EXPECT_TRUE(line.error().contains("factors")) << line.error();
}

TEST(ParseIesTiltLineTest, FailsOnACountWithoutAsManyValues) {
  for (const std::string_view data :
       {"1\n0\n", "1\n-2\n", "1\n3\n0 90 180\n1"}) {
    const std::string profile =
        std::format("{}TILT=INCLUDE\n{}", kHeader, data);
    const auto line = ParseIesTiltLine(profile);
    ASSERT_FALSE(line.has_value()) << data;
    EXPECT_TRUE(line.error().contains("count")) << line.error();
  }
}

TEST(ParseIesTiltLineTest, FailsOnACountThatIsNotAnInteger) {
  const std::string profile = std::format(
      "{}TILT=INCLUDE\n1\n2.0\n0 90\n1 1\n{}", kHeader, kPhotometry);
  const auto line = ParseIesTiltLine(profile);
  ASSERT_FALSE(line.has_value());
  EXPECT_TRUE(line.error().contains("integer")) << line.error();
}

// The photometry starts after included data only when INCLUDE follows the
// equals sign at once.
TEST(ParseIesTiltLineTest, ReadsIncludeAfterASpaceAsTheNameOfAFile) {
  const std::string profile =
      std::format("{}TILT= INCLUDE\n{}", kHeader, kPhotometry);
  const auto line = ParseIesTiltLine(profile);
  ASSERT_TRUE(line.has_value()) << line.error();
  EXPECT_EQ(*line, IesTiltLine(IesTiltFile{.name = "INCLUDE"}));
}

TEST(ParseIesTiltFileTest, ReadsItsData) {
  const auto tilt = ParseIesTiltFile("1\n3\n0 90 180\n1 0.6 0.2\n");
  ASSERT_TRUE(tilt.has_value()) << tilt.error();
  EXPECT_EQ(*tilt, kTilt);
}

TEST(ParseIesTiltFileTest, FailsOnACountThatDoesNotMatchItsValues) {
  for (const std::string_view text :
       {"1\n3\n0 90 180\n1 0.6\n", "1\n2\n0 90 180\n1 0.6 0.2\n"}) {
    const auto tilt = ParseIesTiltFile(text);
    ASSERT_FALSE(tilt.has_value()) << text;
    EXPECT_TRUE(tilt.error().contains("count")) << tilt.error();
  }
}

TEST(ParseIesTiltFileTest, FailsOnAValueThatIsNotANumber) {
  EXPECT_FALSE(ParseIesTiltFile("1\n2\n0 ninety\n1 1\n").has_value());
}

}  // namespace
}  // namespace umbra::delegate::internal
