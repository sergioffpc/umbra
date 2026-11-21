# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Baseline warnings of every umbra target, all of them errors. The safety
# warnings (buffers, lifetimes, thread safety, nullability, conversions) live
# in UmbraSafety.cmake.
#
# The settings apply through umbra_apply_warnings(<target>), called by the
# helpers in UmbraModules.cmake.

include_guard(GLOBAL)

function(umbra_apply_warnings target)
  # The nullability qualifiers of the safety header are a clang extension
  # that umbra adopts on purpose.
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Wno-nullability-extension)
  set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
endfunction()
