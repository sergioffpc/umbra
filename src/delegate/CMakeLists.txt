# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

find_package(pxr CONFIG REQUIRED)
find_package(OpenSubdiv 3.6 CONFIG REQUIRED)

# OpenUSD's IES parser, pxrIES (Cycles' LM-63 parser and USD's photometric
# layer), which OpenUSD builds only into hdEmbree and does not install: umbra
# builds its sources from those the dependency prefix keeps in src/OpenUSD/.
# Its headers are reached through a link under the build tree, at the path
# they include each other by, so that OpenUSD's other sources stay hidden.
find_path(UMBRA_PXR_IES_DIR pxrIES.cpp
          PATH_SUFFIXES src/OpenUSD/pxr/imaging/plugin/hdEmbree/pxrIES
          NO_DEFAULT_PATH PATHS ${CMAKE_PREFIX_PATH} ENV CMAKE_PREFIX_PATH
          REQUIRED)
set(umbra_pxr_ies_root "${CMAKE_CURRENT_BINARY_DIR}/pxr-ies")
set(umbra_pxr_ies_link
    "${umbra_pxr_ies_root}/pxr/imaging/plugin/hdEmbree/pxrIES")
cmake_path(GET umbra_pxr_ies_link PARENT_PATH umbra_pxr_ies_parent)
file(MAKE_DIRECTORY "${umbra_pxr_ies_parent}")
file(CREATE_LINK "${UMBRA_PXR_IES_DIR}" "${umbra_pxr_ies_link}" SYMBOLIC)
set(umbra_pxr_ies_sources "${UMBRA_PXR_IES_DIR}/ies.cpp"
                          "${UMBRA_PXR_IES_DIR}/pxrIES.cpp")

umbra_add_module(
  delegate
  EXCEPTIONS
  SOURCES
  camera_prim.cc
  commit.cc
  denoiser.cc
  dicing.cc
  host_copy.cc
  hydra.cc
  ies_profile.cc
  ies_tilt.cc
  instancer.cc
  light.cc
  material.cc
  mesh.cc
  mesh_prim.cc
  motion.cc
  motion_samples.cc
  outputs.cc
  render_pass.cc
  render_settings_prim.cc
  render_task.cc
  session.cc
  subdivision.cc
  unsupported_prim.cc
  views.cc
  ${umbra_pxr_ies_sources}
  DEPENDS
  core
  device
  ext::ocio
  ext::oidn
  ext::usd
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
  usdImaging
  OpenSubdiv::osdCPU)

# pxrIES is OpenUSD's code, not umbra's: it is compiled without umbra's
# warnings, which are errors. ies.cpp calls strtod and strtol without
# including <cstdlib>, which libc++ does not include for it.
target_include_directories(umbra_delegate SYSTEM PRIVATE
                           "${umbra_pxr_ies_root}")
set_source_files_properties(${umbra_pxr_ies_sources} PROPERTIES
                            COMPILE_OPTIONS "-w;-include;cstdlib")
