# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

find_package(embree 4 CONFIG REQUIRED)

umbra_add_ext_module(embree SOURCES embree.cc DEPENDS core LINK embree)
