# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for libtiff (cmake -C): the shared C library alone, without
# the C++ stream API, the tools, the tests, the contrib code or the docs. The
# common settings of the layer come from layers/deps.cmake.
#
# libtiff is a C library, but it is a submodule: Ubuntu's libtiff6 links
# liblerc4, whose LERC codec is C++ against libstdc++. This build leaves LERC
# out, and with it the two other codecs no image umbra reads uses: JBIG
# (bilevel fax) and LZMA. The codecs it keeps come from the Ubuntu packages
# of their C libraries: zlib and libdeflate (Deflate and PixarLog), libjpeg
# (JPEG and old JPEG), zstd and libwebp; the rest are built in.

set(tiff-cxx OFF CACHE BOOL "")
set(tiff-tools OFF CACHE BOOL "")
set(tiff-tests OFF CACHE BOOL "")
set(tiff-contrib OFF CACHE BOOL "")
set(tiff-docs OFF CACHE BOOL "")
set(tiff-static OFF CACHE BOOL "")
set(tiff-install ON CACHE BOOL "")
set(tiff-opengl OFF CACHE BOOL "")
set(sphinx OFF CACHE BOOL "")

set(lerc OFF CACHE BOOL "")
set(jbig OFF CACHE BOOL "")
set(lzma OFF CACHE BOOL "")
set(zlib ON CACHE BOOL "")
set(libdeflate ON CACHE BOOL "")
set(pixarlog ON CACHE BOOL "")
set(jpeg ON CACHE BOOL "")
set(old-jpeg ON CACHE BOOL "")
set(zstd ON CACHE BOOL "")
set(webp ON CACHE BOOL "")
