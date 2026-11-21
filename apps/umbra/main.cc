// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The umbra executable. It prints its version block, in the style of LLVM's
// tools:
//
//   umbra version 0.1.0 (https://github.com/sergioffpc/umbra <commit>)
//   Build: Release, x86-64-v3, clang 23.1.6
//   License: Apache-2.0 — provided "AS IS", WITHOUT WARRANTIES OR ...

#include <print>

#include "umbra/core/build_info.h"
#include "umbra/core/floating_point.h"
#include "umbra/core/version.h"

int main() {
  umbra::core::ConfigureThreadFloatingPoint();

  const umbra::core::BuildInfo build = umbra::core::GetBuildInfo();
  // UMBRA_HOMEPAGE_URL is the homepage of the CMake project, defined by
  // apps/umbra/CMakeLists.txt.
  std::println("umbra version {} ({} {})", umbra::core::Version(),
               UMBRA_HOMEPAGE_URL, build.commit);
  std::println("Build: {}, {}, {}", build.build_type, build.arch,
               build.compiler);
  std::println(
      "License: Apache-2.0 — provided \"AS IS\", WITHOUT WARRANTIES OR "
      "CONDITIONS OF ANY KIND.");
  return 0;
}
