# SPDX-License-Identifier: Apache-2.0
# Copyright The Umbra Authors

umbra_add_executable(umbra SOURCES main.cc DEPENDS core)
target_compile_definitions(umbra PRIVATE
                           UMBRA_HOMEPAGE_URL="${PROJECT_HOMEPAGE_URL}")
