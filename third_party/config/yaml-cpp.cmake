# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for yaml-cpp (cmake -C): the shared library alone, which
# OpenColorIO reads its configs with, without the contrib code, the parse
# tools or the tests. The common settings of the layer come from
# layers/deps.cmake. The API reports errors with exceptions; OpenColorIO
# catches them.

set(YAML_BUILD_SHARED_LIBS ON CACHE BOOL "")
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "")
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "")
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "")
set(YAML_CPP_INSTALL ON CACHE BOOL "")
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "")
