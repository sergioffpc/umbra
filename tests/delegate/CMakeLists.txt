# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# The tests build the delegate's sources that hold no OpenUSD type
# themselves, rather than link the delegate and OpenUSD with it: they see its
# public headers without linking it.

umbra_add_test(
  umbra_delegate_tests
  SOURCES
  buffer_layout_test.cc
  device_threads_test.cc
  dicing_test.cc
  ies_tilt_test.cc
  motion_samples_test.cc
  render_vars_test.cc
  "${PROJECT_SOURCE_DIR}/src/delegate/buffer_layout.cc"
  "${PROJECT_SOURCE_DIR}/src/delegate/device_threads.cc"
  "${PROJECT_SOURCE_DIR}/src/delegate/dicing.cc"
  "${PROJECT_SOURCE_DIR}/src/delegate/ies_tilt.cc"
  "${PROJECT_SOURCE_DIR}/src/delegate/motion_samples.cc"
  DEPENDS
  device)
target_include_directories(umbra_delegate_tests PRIVATE
                           "${PROJECT_SOURCE_DIR}/src/delegate")
umbra_see_headers(umbra_delegate_tests PRIVATE delegate)
