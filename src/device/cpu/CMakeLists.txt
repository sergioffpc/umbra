# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

umbra_add_module(
  device/cpu
  SOURCES
  bvh.cc
  copies.cc
  cpu_device.cc
  DEPENDS
  core
  device
  ext::embree)
