// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The interface between umbra and its GPU module, libumbra-gpu.so, which umbra
// loads at run time with dlopen (see module.h). The module exports one C
// symbol, kModuleEntryPoint, a function that returns its ModuleApi. Both sides
// come from the same build, so the table may use C++ types; abi_version only
// guards against a module left over from another build.

#ifndef UMBRA_GPU_MODULE_ABI_H_
#define UMBRA_GPU_MODULE_ABI_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::gpu {

// The version of ModuleApi. Bump it whenever ModuleApi or ModuleStatus
// changes.
inline constexpr std::uint32_t kModuleAbiVersion = 1;

// The name of the exported entry point, of type ModuleEntryPoint.
inline constexpr const char* kModuleEntryPoint = "umbra_gpu_module";

// Whether a module call succeeded.
enum class ModuleOutcome : std::uint8_t {
  kOk,
  kFailed,
};

// The outcome of a module call and, when it failed, its error message
// (NUL-terminated, truncated to fit).
struct ModuleStatus {
  ModuleOutcome outcome = ModuleOutcome::kOk;
  std::array<char, 256> message{};
};

// A failed status with `message`, truncated to fit.
inline ModuleStatus FailedStatus(std::string_view message) {
  ModuleStatus status{.outcome = ModuleOutcome::kFailed};
  std::ranges::copy(message.substr(0, status.message.size() - 1),
                    status.message.begin());
  return status;
}

// The functions of the module.
struct ModuleApi {
  // kModuleAbiVersion of the build that produced the module.
  std::uint32_t abi_version;

  // Writes the number of usable GPUs (sm_120 or newer, with a driver whose
  // OptiX runtime the module can use) to *count. Fails when the driver or
  // OptiX cannot be initialized.
  ModuleStatus (*count_devices)(std::int32_t* count);
};

// The type of the module's entry point.
using ModuleEntryPoint = const ModuleApi* UMBRA_NULLABLE (*)();

}  // namespace umbra::gpu

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_GPU_MODULE_ABI_H_
