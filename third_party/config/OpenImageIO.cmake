# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for OpenImageIO (cmake -C): libOpenImageIO and
# libOpenImageIO_Util, shared, with the format plugins embedded, without the
# tools, the tests, the bindings, the viewer, the docs or the fonts. The
# common settings of the layer come from layers/deps.cmake.
#
# Every dependency comes from the layer or from Ubuntu, and OpenImageIO
# builds none of its own (BUILD_MISSING_DEPS and BUILD_LOCAL_DEPS empty).
# OpenImageIO drops an optional dependency it cannot find without failing,
# so the ones umbra wants are required.
#
#   layer    Imath, OpenEXR, OpenColorIO, libtiff, pugixml, {fmt},
#            robin-map, oneTBB, Ptex
#   Ubuntu   libjpeg-turbo, libpng, libwebp, giflib, OpenJPEG, FreeType,
#            zlib, libdeflate, zstd
#
# The formats whose libraries are C++ in Ubuntu (JPEG XL, HEIF/AVIF, camera
# RAW, DICOM, video, Ultra HDR) stay out, with OpenVDB: umbra loads volumes
# itself. The Ptex plugin reads .ptx files, each face a subimage; the
# TextureSystem does no per-face Ptex filtering. OpenImageIO has no GPU code;
# its TextureSystem runs on the CPU. The API reports errors through return
# values and geterror(), not exceptions.

set(OIIO_BUILD_TOOLS OFF CACHE BOOL "")
set(OIIO_BUILD_TESTS OFF CACHE BOOL "")
set(BUILD_TESTING OFF CACHE BOOL "")
set(BUILD_DOCS OFF CACHE BOOL "")
set(INSTALL_DOCS OFF CACHE BOOL "")
set(INSTALL_FONTS OFF CACHE BOOL "")
set(EMBEDPLUGINS ON CACHE BOOL "")
set(USE_PYTHON OFF CACHE BOOL "")
set(USE_QT OFF CACHE BOOL "")
set(OIIO_USE_HWY OFF CACHE BOOL "")
set(USE_CCACHE OFF CACHE BOOL "")
set(STOP_ON_WARNING OFF CACHE BOOL "")

set(OpenImageIO_BUILD_MISSING_DEPS "" CACHE STRING "")
set(OpenImageIO_BUILD_LOCAL_DEPS "" CACHE STRING "")
set(OpenImageIO_REQUIRED_DEPS
    "TIFF;JPEG;PNG;WebP;GIF;OpenJPEG;Freetype;OpenColorIO;TBB;pugixml;fmt;Robinmap;Ptex"
    CACHE STRING "")

# The parallel_for of OpenImageIO runs on oneTBB when the application sets
# OIIO::attribute("use_tbb", 1), sharing the process's thread pool.
# pugixml is the layer's shared library instead of an embedded copy.
set(USE_EXTERNAL_PUGIXML ON CACHE BOOL "")
# libfmt of the layer, compiled, instead of a copy of its headers inside
# OpenImageIO's: one fmt in the process. Consumers link libfmt too.
set(OIIO_USE_COMPILED_FMT ON CACHE BOOL "")
set(OIIO_INTERNALIZE_FMT OFF CACHE BOOL "")

# Ubuntu ships libjpeg-turbo without its CMake package: OpenImageIO finds it
# as plain JPEG. openjph (HTJ2K) is internal to OpenEXR.
foreach(package JXL Libheif LibRaw libuhdr DCMTK FFmpeg OpenCV OpenVDB
                R3DSDK Nuke hwy OpenGL Qt5 Qt6 libjpeg-turbo openjph)
  set(USE_${package} OFF CACHE BOOL "")
endforeach()
