# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

find_package(embree 4 CONFIG REQUIRED)

umbra_add_ext_module(
  embree
  SOURCES
  bvh.cc
  copies.cc
  cpu_device.cc
  embree.cc
  DEPENDS
  core
  device
  LINK
  embree)
# CpuDevice derives from device::Device, which umbra builds without RTTI: its
# source is built without RTTI either, so that it needs no type_info of it.
set_source_files_properties(cpu_device.cc PROPERTIES COMPILE_OPTIONS -fno-rtti)
