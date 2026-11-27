# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

find_package(OpenImageDenoise 2 CONFIG REQUIRED)

umbra_add_ext_module(oidn SOURCES oidn.cc DEPENDS core
                     LINK OpenImageDenoise)
