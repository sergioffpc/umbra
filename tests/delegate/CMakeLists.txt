# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# The tests build the delegate's sources that hold no OpenUSD type
# themselves, rather than link the delegate and OpenUSD with it.

umbra_add_test(
  umbra_delegate_tests
  SOURCES
  dicing_test.cc
  ies_tilt_test.cc
  motion_samples_test.cc
  "${PROJECT_SOURCE_DIR}/src/delegate/dicing.cc"
  "${PROJECT_SOURCE_DIR}/src/delegate/ies_tilt.cc"
  "${PROJECT_SOURCE_DIR}/src/delegate/motion_samples.cc"
  DEPENDS
  device)
target_include_directories(umbra_delegate_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/src/delegate")
