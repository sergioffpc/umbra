# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for Imath (cmake -C): the vector, matrix and half types that
# OpenEXR, OpenColorIO, OpenImageIO, OpenVDB and OSL share, as libImath
# (the half-to-float table), without Python bindings, tests or website. The
# common settings of the layer come from layers/deps.cmake.
#
# A few templates throw (Frustum, and Matrix inversion when asked to report a
# singular matrix); code built with -fno-exceptions cannot instantiate them.
# Half conversion uses F16C under x86-64-v3, the table only without it.

set(BUILD_TESTING OFF CACHE BOOL "")
set(PYTHON OFF CACHE BOOL "")
set(PYBIND11 OFF CACHE BOOL "")
set(BUILD_WEBSITE OFF CACHE BOOL "")
