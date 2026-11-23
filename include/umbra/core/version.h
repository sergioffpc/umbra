// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#ifndef UMBRA_CORE_VERSION_H_
#define UMBRA_CORE_VERSION_H_

#include <string_view>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::core {

// Returns the version of umbra, "MAJOR.MINOR.PATCH", as declared in the CMake
// project.
std::string_view Version();

}  // namespace umbra::core

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_CORE_VERSION_H_
