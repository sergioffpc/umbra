# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Runtime hardening. libc++ checks its preconditions in debug mode in Debug
# builds, so tests catch library misuse, and not at all in Release builds, so
# production pays no runtime cost.
#
# The settings apply through umbra_apply_hardening(<target>), called by the
# helpers in UmbraModules.cmake.

include_guard(GLOBAL)

function(umbra_apply_hardening target)
  set(mode "$<IF:$<CONFIG:Debug>,DEBUG,NONE>")
  target_compile_definitions(${target} PRIVATE
    _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_${mode})
endfunction()
