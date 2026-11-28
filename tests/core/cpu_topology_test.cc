// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/core/cpu_topology.h"

#include <gtest/gtest.h>

#include <optional>

namespace umbra::core {
namespace {

TEST(ParseNumaNodeTest, IsTheNodeOfANonNegativeDecimal) {
  EXPECT_EQ(ParseNumaNode("0"), std::optional<int>(0));
  EXPECT_EQ(ParseNumaNode("1\n"), std::optional<int>(1));
}

// The kernel writes -1 for a device attached to no node, as on a machine
// with one node.
TEST(ParseNumaNodeTest, IsNoNodeForANegativeValue) {
  EXPECT_EQ(ParseNumaNode("-1"), std::nullopt);
  EXPECT_EQ(ParseNumaNode("-1\n"), std::nullopt);
}

TEST(ParseNumaNodeTest, IsNoNodeForTextThatIsNotANode) {
  EXPECT_EQ(ParseNumaNode(""), std::nullopt);
  EXPECT_EQ(ParseNumaNode("abc"), std::nullopt);
  // One past the largest int.
  EXPECT_EQ(ParseNumaNode("2147483648"), std::nullopt);
}

}  // namespace
}  // namespace umbra::core
