# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

find_package(OSL 1.15 CONFIG REQUIRED)

umbra_add_ext_module(osl SOURCES osl.cc DEPENDS core LINK OSL::oslexec)
