// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The check that the CPU runs x86-64-v3, the architecture every build
// targets. On a CPU without it, the first v3 instruction would kill umbra
// with an illegal instruction, before main and before any message.
//
// The check runs from umbra's .preinit_array, which the dynamic loader calls
// before the initializers of every library and of umbra itself. This file is
// compiled for the baseline x86-64 (apps/umbra/CMakeLists.txt), the check is
// not sanitized, and it calls only compiler-rt's CPU detection and libc's
// write and _exit, which do not use v3 instructions either. In sanitizer
// builds the sanitizer runtime's own .preinit_array entry may run first.

#include <unistd.h>

#include <string_view>

namespace {

// The message names the features of x86-64-v3 that umbra uses.
static_assert(std::string_view(UMBRA_ARCH) == "x86-64-v3",
              "the CPU check knows only x86-64-v3");

// The exit status of a failure (docs/adr/0002-cli-diagnostics-on-stderr.md).
constexpr int kExitFailure = 1;

[[clang::no_sanitize("address", "thread", "undefined", "integer",
                     "implicit-conversion", "nullability")]]
void CheckCpu(int /*argc*/, char** /*argv*/, char** /*envp*/) {
  __builtin_cpu_init();
  if (__builtin_cpu_supports(UMBRA_ARCH)) {
    return;
  }
  constexpr std::string_view kMessage =
      "fatal: this CPU lacks " UMBRA_ARCH
      " (AVX2, FMA, BMI2), which umbra is built for\n";
  // Nothing is left to report a failed write to.
  static_cast<void>(write(STDERR_FILENO, kMessage.data(), kMessage.size()));
  _exit(kExitFailure);
}

// The entry of .preinit_array, which the loader calls. The pointer is const;
// the check flags the function it points to, which cannot be.
[[gnu::used, gnu::section(".preinit_array")]]
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): is const
void (*const kPreinitCheckCpu)(int, char**, char**) = CheckCpu;

}  // namespace
