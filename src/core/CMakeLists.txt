# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

umbra_add_module(core
  SOURCES
    cpu_topology.cc
    error.cc
    floating_point.cc
    log.cc
    number_format.cc
    progress.cc
    stderr_output.cc
    version.cc)

# Only version.cc sees the version, so a version bump recompiles one file.
set_source_files_properties(version.cc PROPERTIES
  COMPILE_DEFINITIONS UMBRA_PROJECT_VERSION="${PROJECT_VERSION}")

umbra_add_build_info(umbra_core TEMPLATE build_info.cc.in)
