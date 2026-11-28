# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

umbra_add_module(
  device/cpu
  SOURCES
  arena.cc
  bvh.cc
  commit.cc
  commit_log.cc
  copies.cc
  cpu_device.cc
  render.cc
  trace.cc
  DEPENDS
  core
  device
  ext::embree)
