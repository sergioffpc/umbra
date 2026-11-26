# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

find_package(pxr CONFIG REQUIRED)

umbra_add_ext_module(
  usd
  SOURCES
  close_scene.cc
  hydra.cc
  instancer.cc
  light.cc
  material.cc
  mesh.cc
  motion.cc
  outputs.cc
  usd.cc
  DEPENDS
  core
  scene
  ext::ocio
  LINK
  tf
  gf
  vt
  work
  plug
  ar
  sdf
  usd
  usdGeom
  usdShade
  usdRender
  hd
  hio
  pxOsd
  hdsi
  usdImaging)
