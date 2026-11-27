# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# The tests build the bridge's sources that hold no OpenUSD type themselves,
# rather than link the bridge and OpenUSD with it.

umbra_add_test(
  umbra_ext_usd_tests
  SOURCES
  ies_tilt_test.cc
  motion_samples_test.cc
  "${PROJECT_SOURCE_DIR}/src/ext/usd/ies_tilt.cc"
  "${PROJECT_SOURCE_DIR}/src/ext/usd/motion_samples.cc"
  DEPENDS
  device)
target_include_directories(umbra_ext_usd_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/src/ext/usd")
