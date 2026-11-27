# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

find_package(embree 4 CONFIG REQUIRED)

umbra_add_ext_module(
  embree
  SOURCES
  conversions.cc
  embree.cc
  DEPENDS
  core
  device
  LINK
  embree)
