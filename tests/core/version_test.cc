// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/core/version.h"

#include <gtest/gtest.h>

namespace umbra::core {
namespace {

// UMBRA_EXPECTED_VERSION is the version declared in the CMake project,
// defined by tests/core/CMakeLists.txt.
TEST(VersionTest, EqualsTheCMakeProjectVersion) {
  EXPECT_EQ(Version(), UMBRA_EXPECTED_VERSION);
}

}  // namespace
}  // namespace umbra::core
