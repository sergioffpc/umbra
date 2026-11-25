# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors
#
# OpenVDB installs a find module, FindOpenVDB.cmake, rather than a package
# config; it is found in the prefixes of CMAKE_PREFIX_PATH.

find_file(UMBRA_FIND_OPENVDB FindOpenVDB.cmake
          PATH_SUFFIXES lib/cmake/OpenVDB REQUIRED)
cmake_path(GET UMBRA_FIND_OPENVDB PARENT_PATH umbra_openvdb_module_dir)
list(APPEND CMAKE_MODULE_PATH "${umbra_openvdb_module_dir}")
find_package(OpenVDB 14 REQUIRED COMPONENTS openvdb)

umbra_add_ext_module(openvdb SOURCES openvdb.cc DEPENDS core
                     LINK OpenVDB::openvdb)
