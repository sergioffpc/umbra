// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/core/version.h"

#include <string_view>

namespace umbra::core {

// UMBRA_PROJECT_VERSION is the version of the CMake project, defined by
// src/core/CMakeLists.txt.
std::string_view Version() { return UMBRA_PROJECT_VERSION; }

}  // namespace umbra::core
