# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

find_package(OpenImageIO 3 CONFIG REQUIRED)

umbra_add_ext_module(oiio SOURCES oiio.cc DEPENDS core
                     LINK OpenImageIO::OpenImageIO)
