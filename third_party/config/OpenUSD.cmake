# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for OpenUSD (cmake -C): the USD libraries, Hydra and
# usdImaging as shared libraries, with MaterialX, OSL and OpenImageIO, without
# a graphics API, Python, tests or documentation. The common settings of the
# layer come from layers/deps.cmake.
#
# umbra is a Hydra render delegate that draws with OptiX: it needs Hydra and
# usdImaging, not Storm. Without GL, Vulkan or Metal, Storm (hdSt), hdx,
# usdImagingGL and usdview are not built, and with them the OpenColorIO
# color correction task of hdx.

set(PXR_BUILD_IMAGING ON CACHE BOOL "")
set(PXR_BUILD_USD_IMAGING ON CACHE BOOL "")
set(PXR_ENABLE_GL_SUPPORT OFF CACHE BOOL "")
set(PXR_ENABLE_VULKAN_SUPPORT OFF CACHE BOOL "")
set(PXR_ENABLE_METAL_SUPPORT OFF CACHE BOOL "")
set(PXR_BUILD_USDVIEW OFF CACHE BOOL "")
set(PXR_BUILD_OPENCOLORIO_PLUGIN OFF CACHE BOOL "")

# C++ only: no pxr Python modules, and so none of the Python command-line
# tools (usdcat, usdchecker, ...). The C++ ones (sdfdump, sdffilter) stay.
set(PXR_ENABLE_PYTHON_SUPPORT OFF CACHE BOOL "")
set(PXR_BUILD_USD_TOOLS ON CACHE BOOL "")

# usdMtlx reads .mtlx files as UsdShade and hdMtlx hands MaterialX networks to
# the render delegate; sdrOsl reads the parameters of .oso shaders into Sdr;
# hioOiio reads textures through OpenImageIO.
set(PXR_ENABLE_MATERIALX_SUPPORT ON CACHE BOOL "")
set(PXR_ENABLE_OSL_SUPPORT ON CACHE BOOL "")
set(PXR_BUILD_OPENIMAGEIO_PLUGIN ON CACHE BOOL "")

# hioOpenVDB serves Storm's volume textures only, and requires Boost: umbra
# loads the .vdb files of OpenVDBAsset prims itself, with OpenVDB and NanoVDB.
set(PXR_ENABLE_OPENVDB_SUPPORT OFF CACHE BOOL "")
set(PXR_ENABLE_PTEX_SUPPORT OFF CACHE BOOL "")
set(PXR_BUILD_EMBREE_PLUGIN OFF CACHE BOOL "")
set(PXR_BUILD_PRMAN_PLUGIN OFF CACHE BOOL "")
set(PXR_BUILD_ALEMBIC_PLUGIN OFF CACHE BOOL "")
set(PXR_BUILD_DRACO_PLUGIN OFF CACHE BOOL "")

set(PXR_BUILD_TESTS OFF CACHE BOOL "")
set(PXR_BUILD_EXAMPLES OFF CACHE BOOL "")
set(PXR_BUILD_TUTORIALS OFF CACHE BOOL "")
set(PXR_BUILD_DOCUMENTATION OFF CACHE BOOL "")

# The layer's ccache is a compiler launcher, which precompiled headers defeat.
set(PXR_ENABLE_PRECOMPILED_HEADERS OFF CACHE BOOL "")
set(PXR_ENABLE_COMPILER_CACHE OFF CACHE BOOL "")
set(PXR_BUILD_MONOLITHIC OFF CACHE BOOL "")
