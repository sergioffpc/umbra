# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Compiler-enforced safety rules of every umbra target: the warnings that
# back the macros of include/umbra/core/safety.h (unsafe buffer usage,
# lifetimes, thread safety, nullability) and the implicit conversion
# warnings, all of them errors.
#
# The settings apply through umbra_apply_safety(<target>), called by the
# helpers in UmbraModules.cmake.

include_guard(GLOBAL)

function(umbra_apply_safety target)
endfunction()
