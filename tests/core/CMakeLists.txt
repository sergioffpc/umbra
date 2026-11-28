# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

umbra_add_test(umbra_core_tests SOURCES cpu_topology_test.cc lazy_format_test.cc
               version_test.cc
               DEPENDS core)
target_compile_definitions(umbra_core_tests PRIVATE
                           UMBRA_EXPECTED_VERSION="${PROJECT_VERSION}")
