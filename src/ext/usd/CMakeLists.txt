# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

find_package(pxr CONFIG REQUIRED)

umbra_add_ext_module(usd SOURCES stage.cc usd.cc DEPENDS core
                     LINK tf work plug sdf usd)
