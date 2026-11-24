# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for MaterialX (cmake -C): shared MaterialXCore,
# MaterialXFormat, MaterialXGenShader and MaterialXGenOsl, plus the standard
# data library, without Python, tools, tests or documentation. The common settings of the layer come from layers/deps.cmake.
#
# OpenUSD's usdMtlx and hdMtlx use Core and Format. umbra runs MaterialX
# materials through OSL: GenOsl emits shaders whose closures umbra's BSDFs
# implement, and OSL compiles them for OptiX. The other generators target
# rasterizers (GLSL, MSL, Slang) or need the MDL SDK, and the render modules
# need a graphics API. The API reports errors with exceptions.

set(MATERIALX_BUILD_SHARED_LIBS ON CACHE BOOL "")
set(MATERIALX_BUILD_MONOLITHIC OFF CACHE BOOL "")
set(MATERIALX_BUILD_GEN_OSL ON CACHE BOOL "")
# The color spaces of a material (a texture's colorspace attribute, say) come
# from the OpenColorIO config, whose transforms OpenColorIO writes as OSL code
# into the generated shaders. Without it only the fixed color spaces of
# MaterialX's own cmlib would exist.
set(MATERIALX_BUILD_OCIO ON CACHE BOOL "")
set(MATERIALX_BUILD_GEN_GLSL OFF CACHE BOOL "")
set(MATERIALX_BUILD_GEN_MSL OFF CACHE BOOL "")
set(MATERIALX_BUILD_GEN_SLANG OFF CACHE BOOL "")
set(MATERIALX_BUILD_GEN_MDL OFF CACHE BOOL "")
set(MATERIALX_BUILD_RENDER OFF CACHE BOOL "")
set(MATERIALX_BUILD_RENDER_PLATFORMS OFF CACHE BOOL "")
set(MATERIALX_BUILD_OIIO OFF CACHE BOOL "")
set(MATERIALX_BUILD_OSOS OFF CACHE BOOL "")
set(MATERIALX_BUILD_DATA_LIBRARY OFF CACHE BOOL "")

set(MATERIALX_BUILD_PYTHON OFF CACHE BOOL "")
set(MATERIALX_BUILD_JS OFF CACHE BOOL "")
set(MATERIALX_BUILD_VIEWER OFF CACHE BOOL "")
set(MATERIALX_BUILD_GRAPH_EDITOR OFF CACHE BOOL "")
set(MATERIALX_BUILD_DOCS OFF CACHE BOOL "")
set(MATERIALX_BUILD_TESTS OFF CACHE BOOL "")
set(MATERIALX_BUILD_BENCHMARK_TESTS OFF CACHE BOOL "")
set(MATERIALX_BUILD_PERFETTO_TRACING OFF CACHE BOOL "")
set(MATERIALX_INSTALL_PYTHON OFF CACHE BOOL "")
set(MATERIALX_INSTALL_RESOURCES OFF CACHE BOOL "")
set(MATERIALX_TEST_RENDER OFF CACHE BOOL "")

# The image's builds are not cached across runs.
set(MATERIALX_BUILD_USE_CCACHE OFF CACHE BOOL "")

# The data library (the standard nodes' definitions and implementations) goes
# with the data of the other dependencies. OpenUSD copies it into its own
# resources at build time, from MaterialXConfig.cmake's MATERIALX_STDLIB_DIR.
set(MATERIALX_INSTALL_STDLIB_PATH share/MaterialX/libraries CACHE STRING "")
