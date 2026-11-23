// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Entry point of every test executable (see umbra_add_test).

#include <gtest/gtest.h>

#include "umbra/core/floating_point.h"

int main(int argc, char** argv) {
  umbra::core::ConfigureThreadFloatingPoint();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
