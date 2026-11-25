# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

find_package(OpenColorIO 2 CONFIG REQUIRED)

umbra_add_ext_module(ocio SOURCES ocio.cc DEPENDS core
                     LINK OpenColorIO::OpenColorIO)
