// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The GPU module, libumbra-gpu.so: the entry point umbra loads (see
// include/umbra/gpu/module_abi.h) over the CUDA runtime and OptiX. It
// describes the GPUs and which of them are usable.

#include <cuda_runtime_api.h>
#include <driver_types.h>
#include <optix.h>
#include <optix_function_table_definition.h>  // IWYU pragma: keep

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "device.h"
#include "support.h"
#include "umbra/core/log.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/gpu/module_abi.h"

namespace umbra::gpu::backend {
namespace {

void SetLog(ModuleLogFunction log, core::LogLevel threshold) {
  Logging().log.store(log, std::memory_order_relaxed);
  Logging().threshold.store(threshold, std::memory_order_relaxed);
}

// Copies `text` into `out`, NUL-terminated and truncated to fit.
template <std::size_t N>
void CopyText(std::string_view text, std::array<char, N>& out) {
  out.fill('\0');
  std::ranges::copy(text.substr(0, N - 1), out.begin());
}

// Logs a GPU: at debug, or at warning when the runtime cannot describe it.
void LogDevice(const GpuDescription& gpu) {
  const std::string_view name = TextOf(gpu.name);
  switch (gpu.usability) {
    case GpuUsability::kUsable:
      Log(core::LogLevel::kDebug, "device {}: {} (sm_{}{})", gpu.ordinal, name,
          gpu.sm_major, gpu.sm_minor);
      return;
    case GpuUsability::kTooOld:
      Log(core::LogLevel::kDebug,
          "device {}: {} (sm_{}{}), skipped: older than sm_{}0", gpu.ordinal,
          name, gpu.sm_major, gpu.sm_minor, kMinComputeCapabilityMajor);
      return;
    case GpuUsability::kNoOptix:
      Log(core::LogLevel::kDebug, "device {}: {}, skipped: no OptiX",
          gpu.ordinal, name);
      return;
    case GpuUsability::kUnreadable:
      Log(core::LogLevel::kWarning,
          "device {}: skipped: the CUDA runtime cannot describe it",
          gpu.ordinal);
      return;
  }
}

// The GPU at `ordinal`, as the CUDA runtime describes it, or kUnreadable
// when it cannot.
GpuDescription Describe(int ordinal) {
  cudaDeviceProp properties{};
  if (cudaGetDeviceProperties(&properties, ordinal) != cudaSuccess) {
    static_cast<void>(cudaGetLastError());
    return {.ordinal = ordinal, .usability = GpuUsability::kUnreadable};
  }
  GpuDescription gpu{
      .ordinal = ordinal,
      .usability = properties.major < kMinComputeCapabilityMajor
                       ? GpuUsability::kTooOld
                       : GpuUsability::kUsable,
      .sm_major = properties.major,
      .sm_minor = properties.minor,
      .memory_bytes = properties.totalGlobalMem,
      .name = {},
      .pci_bus_id = {},
  };
  CopyText(std::string_view(std::ranges::begin(properties.name),
                            std::ranges::find(properties.name, '\0')),
           gpu.name);
  if (cudaDeviceGetPCIBusId(gpu.pci_bus_id.data(),
                            static_cast<int>(gpu.pci_bus_id.size()),
                            ordinal) != cudaSuccess) {
    gpu.pci_bus_id.fill('\0');
  }
  return gpu;
}

// Logs the versions of the driver and OptiX the module runs on.
void LogVersions() {
  int driver = 0;
  if (cudaDriverGetVersion(&driver) != cudaSuccess) {
    driver = 0;
  }
  Log(core::LogLevel::kDebug, "CUDA driver {}.{}, OptiX {}.{}.{}",
      driver / 1000, driver % 1000 / 10, OPTIX_VERSION / 10000,
      OPTIX_VERSION / 100 % 100, OPTIX_VERSION % 100);
}

// The described GPUs of `inventory`, to change.
std::span<GpuDescription> GpusOf(GpuInventory& inventory) {
  return std::span(inventory.gpus)
      .first(static_cast<std::size_t>(inventory.count));
}

// Marks the usable GPUs kNoOptix when the driver's OptiX cannot be used.
void CheckOptix(GpuInventory& inventory) {
  const auto usable = [](const GpuDescription& gpu) {
    return gpu.usability == GpuUsability::kUsable;
  };
  if (std::ranges::none_of(GpusOf(inventory), usable)) {
    return;
  }
  const Result<void> optix = InitOptix();
  if (optix.has_value()) {
    LogVersions();
    return;
  }
  CopyText(optix.error(), inventory.optix_error);
  for (GpuDescription& gpu : GpusOf(inventory)) {
    if (usable(gpu)) {
      gpu.usability = GpuUsability::kNoOptix;
    }
  }
}

ModuleStatus DescribeDevices(GpuInventory* inventory) {
  *inventory = {};
  int count = 0;
  if (auto checked = CheckCuda(cudaGetDeviceCount(&count), "no device");
      !checked) {
    return FailedStatus(checked.error());
  }
  if (std::cmp_greater(count, kMaxGpus)) {
    Log(core::LogLevel::kWarning, "{} devices; describing the first {}", count,
        kMaxGpus);
    count = static_cast<int>(kMaxGpus);
  }
  for (int ordinal = 0; ordinal < count; ++ordinal) {
    inventory->gpus.at(static_cast<std::size_t>(ordinal)) = Describe(ordinal);
  }
  inventory->count = count;
  CheckOptix(*inventory);
  for (const GpuDescription& gpu : DescribedGpus(*inventory)) {
    LogDevice(gpu);
  }
  if (cudaDriverGetVersion(&inventory->cuda_driver_version) != cudaSuccess) {
    inventory->cuda_driver_version = 0;
  }
  inventory->optix_version = OPTIX_VERSION;
  return {};
}

ModuleDevice* UMBRA_NULLABLE CreateDevice(std::int32_t ordinal,
                                          ModuleStatus* status) {
  auto made = ModuleDevice::Create(ordinal);
  if (!made.has_value()) {
    *status = FailedStatus(made.error());
    return nullptr;
  }
  *status = {};
  return made->release();
}

void DestroyDevice(ModuleDevice* destroyed) {
  const std::unique_ptr<ModuleDevice> owned(destroyed);
}

ModuleStatus Commit(ModuleDevice* gpu, const device::ChangeSet* changes) {
  if (auto committed = gpu->Commit(*changes); !committed.has_value()) {
    return FailedStatus(committed.error());
  }
  return {};
}

constexpr ModuleApi kApi{
    .abi_version = kModuleAbiVersion,
    .set_log = SetLog,
    .describe_devices = DescribeDevices,
    .create_device = CreateDevice,
    .destroy_device = DestroyDevice,
    .commit = Commit,
};

}  // namespace
}  // namespace umbra::gpu::backend

extern "C" [[gnu::visibility("default")]] const umbra::gpu::ModuleApi*
umbra_gpu_module() {  // NOLINT(readability-identifier-naming): C symbol.
  return &umbra::gpu::backend::kApi;
}
