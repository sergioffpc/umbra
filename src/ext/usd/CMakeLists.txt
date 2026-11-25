# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

find_package(pxr CONFIG REQUIRED)

umbra_add_ext_module(
  usd
  SOURCES
  close_scene.cc
  hydra.cc
  outputs.cc
  usd.cc
  DEPENDS
  core
  scene
  LINK
  tf
  gf
  vt
  work
  plug
  sdf
  usd
  usdGeom
  usdRender
  hd
  hdsi
  usdImaging)
