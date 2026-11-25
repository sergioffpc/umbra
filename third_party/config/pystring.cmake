# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for pystring (cmake -C): the shared library, Python's string
# methods for C++, which OpenColorIO uses. The common settings of the layer
# come from layers/deps.cmake. pystring has no options of its own: it always
# builds its test programs, which it does not install.

set(BUILD_SHARED_LIBS ON CACHE BOOL "")
