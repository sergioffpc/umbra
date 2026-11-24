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
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "umbra/core/log.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::gpu {

// The version of ModuleApi. Bump it whenever ModuleApi or ModuleStatus
// changes.
inline constexpr std::uint32_t kModuleAbiVersion = 3;

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

// umbra's diagnostics, as the loader hands them to the module: the module
// keeps no logging state of its own and logs through this function, as the
// gpu component.
using ModuleLogFunction = void (*)(core::LogLevel level,
                                   std::string_view message);

// Whether the module can run on a GPU, or why not.
enum class GpuUsability : std::uint8_t {
  kUsable,
  // Its architecture is older than the oldest the module is built for.
  kTooOld,
  // The driver's OptiX runtime cannot be used; GpuInventory says why.
  kNoOptix,
  // The CUDA runtime cannot describe it.
  kUnreadable,
};

// One GPU, as the CUDA runtime describes it.
struct GpuDescription {
  // The CUDA ordinal.
  std::int32_t ordinal = 0;
  GpuUsability usability = GpuUsability::kUsable;
  std::int32_t sm_major = 0;
  std::int32_t sm_minor = 0;
  std::uint64_t memory_bytes = 0;
  // NUL-terminated, truncated to fit.
  std::array<char, 256> name{};
  // The PCI address, "0000:01:00.0", NUL-terminated.
  std::array<char, 32> pci_bus_id{};
};

// The most GPUs a module describes.
inline constexpr std::size_t kMaxGpus = 64;

// Every GPU the CUDA runtime sees, in CUDA order, and what they run on.
struct GpuInventory {
  std::int32_t count = 0;
  std::array<GpuDescription, kMaxGpus> gpus{};
  // cudaDriverGetVersion: 13040 for CUDA 13.4.
  std::int32_t cuda_driver_version = 0;
  // OPTIX_VERSION of the module: 90100 for OptiX 9.1.0.
  std::int32_t optix_version = 0;
  // Why OptiX cannot be used, for GPUs that are kNoOptix; NUL-terminated.
  std::array<char, 256> optix_error{};
};

// The text of a NUL-terminated array of the ABI.
template <std::size_t N>
constexpr std::string_view TextOf(const std::array<char, N>& text) {
  return {text.begin(), std::ranges::find(text, '\0')};
}

// The GPUs `inventory` describes.
inline std::span<const GpuDescription> DescribedGpus(
    const GpuInventory& inventory) {
  return std::span(inventory.gpus)
      .first(std::min(static_cast<std::size_t>(std::max(inventory.count, 0)),
                      kMaxGpus));
}

// The functions of the module.
struct ModuleApi {
  // kModuleAbiVersion of the build that produced the module.
  std::uint32_t abi_version;

  // Hands the module umbra's log function and the gpu component's threshold,
  // so that it formats only the messages that will be shown. The loader
  // calls it once, before any other function.
  void (*set_log)(ModuleLogFunction log, core::LogLevel threshold);

  // Describes every GPU the CUDA runtime sees in *inventory, usable or not:
  // a GPU is usable when it is sm_120 or newer and the module can use the
  // driver's OptiX runtime.
  // Fails when the CUDA runtime cannot be initialized.
  ModuleStatus (*describe_devices)(GpuInventory* inventory);
};

// The type of the module's entry point.
using ModuleEntryPoint = const ModuleApi* UMBRA_NULLABLE (*)();

}  // namespace umbra::gpu

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_GPU_MODULE_ABI_H_
