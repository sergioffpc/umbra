// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The GPU module, libumbra-gpu.so: the entry point umbra loads (see
// include/umbra/device/gpu/module_abi.h) over the CUDA runtime and OptiX,
// through their bridges. It describes the GPUs and which of them are usable.

#include <driver_types.h>
#include <optix.h>

#include <algorithm>
#include <array>
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
#include "umbra/device/device.h"
#include "umbra/device/gpu/module_abi.h"
#include "umbra/device/image.h"
#include "umbra/ext/cuda/errors.h"
#include "umbra/ext/cuda/runtime.h"
#include "umbra/ext/optix/errors.h"
#include "umbra/ext/optix/optix.h"

namespace umbra::device::gpu::backend {
namespace {

void SetLog(ModuleLogFunction log, core::LogLevel threshold) {
  Logging().Set(log, threshold);
  ext::cuda::SetLog(log, threshold);
  ext::optix::SetLog(log, threshold);
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
  const auto read = ext::cuda::DeviceProperties(ordinal, "cannot describe it");
  if (!read.has_value()) {
    return {.ordinal = ordinal, .usability = GpuUsability::kUnreadable};
  }
  const cudaDeviceProp& properties = *read;
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
  if (!ext::cuda::PciBusId(ordinal, gpu.pci_bus_id, "cannot read its bus id")
           .has_value()) {
    gpu.pci_bus_id.fill('\0');
  }
  return gpu;
}

// Logs the versions of the driver and OptiX the module runs on.
void LogVersions() {
  const int driver =
      ext::cuda::DriverVersion("cannot read the driver's version").value_or(0);
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
  const Result<void> optix = ext::optix::Initialize();
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
  const auto counted = ext::cuda::DeviceCount("no device");
  if (!counted.has_value()) {
    return FailedStatus(counted.error());
  }
  int count = *counted;
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
  inventory->cuda_driver_version =
      ext::cuda::DriverVersion("cannot read the driver's version").value_or(0);
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

// A module status of `result`.
ModuleStatus StatusOf(const Result<void>& result) {
  return result.has_value() ? ModuleStatus() : FailedStatus(result.error());
}

ModuleStatus PrepareRender(ModuleDevice* gpu,
                           const device::FrameParameters* frame) {
  return StatusOf(gpu->PrepareRender(*frame));
}

ModuleStatus RenderTiles(ModuleDevice* gpu,
                         const device::FrameParameters* frame,
                         std::span<const device::PixelWindow> tiles) {
  return StatusOf(gpu->RenderTiles(*frame, tiles));
}

constexpr ModuleApi kApi{
    .abi_version = kModuleAbiVersion,
    .set_log = SetLog,
    .describe_devices = DescribeDevices,
    .create_device = CreateDevice,
    .destroy_device = DestroyDevice,
    .commit = Commit,
    .prepare_render = PrepareRender,
    .render_tiles = RenderTiles,
};

}  // namespace
}  // namespace umbra::device::gpu::backend

extern "C" [[gnu::visibility("default")]] const umbra::device::gpu::ModuleApi*
umbra_gpu_module() {  // NOLINT(readability-identifier-naming): C symbol.
  return &umbra::device::gpu::backend::kApi;
}
