# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

find_package(OpenSubdiv 3.6 CONFIG REQUIRED)

umbra_add_ext_module(opensubdiv SOURCES opensubdiv.cc DEPENDS core
                     LINK OpenSubdiv::osdCPU)
