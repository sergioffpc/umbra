# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for pugixml (cmake -C): the shared library and its CMake
# package, which OpenImageIO and OSL read their XML with, instead of each
# embedding a copy. The common settings of the layer come from
# layers/deps.cmake. Its API reports XML errors in parse results; exceptions
# stay enabled, as the default build that both projects expect.

set(PUGIXML_INSTALL ON CACHE BOOL "")
set(PUGIXML_BUILD_TESTS OFF CACHE BOOL "")
