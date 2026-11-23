// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The umbra executable. It prints its version block, in the style of LLVM's
// tools, and the GPU executors it can run (see include/umbra/gpu/module.h):
//
//   umbra version 0.1.0 (https://github.com/sergioffpc/umbra <commit>)
//   Build: Release, x86-64-v3, clang 23.1.6
//   License: Apache-2.0 — provided "AS IS", WITHOUT WARRANTIES OR ...
//   GPU executors: 1
//
// or, without a usable GPU, "GPU executors: none (<reason>)".

#include <print>

#include "umbra/core/build_info.h"
#include "umbra/core/floating_point.h"
#include "umbra/core/version.h"
#include "umbra/gpu/module.h"

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

  const auto gpus = umbra::gpu::DefaultModulePath().and_then(
      [](const auto& path) { return umbra::gpu::CountGpuExecutors(path); });
  if (gpus.has_value() && *gpus > 0) {
    std::println("GPU executors: {}", *gpus);
  } else if (gpus.has_value()) {
    std::println("GPU executors: none (no usable GPU)");
  } else {
    std::println("GPU executors: none ({})", gpus.error().message());
  }
  return 0;
}
