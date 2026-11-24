# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# Initial cache for Imath (cmake -C): the vector, matrix and half types that
# OpenEXR, OpenColorIO, OpenImageIO, OpenVDB and OSL share, as libImath
# (the half-to-float table), without Python bindings, tests or website. The
# common settings of the layer come from layers/deps.cmake.
#
# The *Exc methods of Frustum and the matrix inversions that take singExc
# throw, so code built with -fno-exceptions cannot call them; their
# counterparts without the suffix or the flag are noexcept.
# Half conversion uses F16C under x86-64-v3, the table only without it.

set(BUILD_TESTING OFF CACHE BOOL "")
set(PYTHON OFF CACHE BOOL "")
set(PYBIND11 OFF CACHE BOOL "")
set(BUILD_WEBSITE OFF CACHE BOOL "")
