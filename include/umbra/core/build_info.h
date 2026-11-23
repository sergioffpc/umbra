// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#ifndef UMBRA_CORE_BUILD_INFO_H_
#define UMBRA_CORE_BUILD_INFO_H_

#include <string_view>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::core {

// How this binary was built. The strings have static storage duration.
struct BuildInfo {
  // Full hash of the commit built, followed by "-dirty" when tracked files
  // had uncommitted changes; "unknown" when built outside a git checkout.
  std::string_view commit;
  // The CMake build type, e.g. "Release".
  std::string_view build_type;
  // The target architecture, e.g. "x86-64-v3".
  std::string_view arch;
  // The compiler name and version, e.g. "clang 23.1.6".
  std::string_view compiler;
};

// Returns how this binary was built.
BuildInfo GetBuildInfo();

}  // namespace umbra::core

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_CORE_BUILD_INFO_H_
