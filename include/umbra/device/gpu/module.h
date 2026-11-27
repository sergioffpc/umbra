// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Loads the GPU module, libumbra-gpu.so, at run time. umbra never links CUDA:
// the GPU backend lives in that module, which is loaded only when it is
// installed, and which describes the GPUs the CUDA runtime sees.

#ifndef UMBRA_DEVICE_GPU_MODULE_H_
#define UMBRA_DEVICE_GPU_MODULE_H_

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/device/device.h"
#include "umbra/device/gpu/module_abi.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device::gpu {

// The file name of the GPU module.
inline constexpr std::string_view kModuleFileName = "libumbra-gpu.so";

// Where umbra looks for the GPU module: lib/libumbra-gpu.so next to the bin/
// directory of the running executable, in the build tree as when installed.
[[nodiscard]] std::expected<std::filesystem::path, core::Error>
DefaultModulePath();

// One GPU, usable or not.
struct GpuDevice {
  // The CUDA ordinal: gpu<ordinal> as a render device.
  int ordinal = 0;
  GpuUsability usability = GpuUsability::kUsable;
  std::string name;
  int sm_major = 0;
  int sm_minor = 0;
  std::uint64_t memory_bytes = 0;
  // The NUMA node the GPU is attached to, when the machine says.
  std::optional<int> numa_node;
};

// Every GPU the CUDA runtime sees, and what they run on.
struct GpuDevices {
  std::vector<GpuDevice> gpus;
  // The kernel driver's version ("615.71"), or empty when unknown.
  std::string driver_version;
  // 13040 for CUDA 13.4.
  int cuda_driver_version = 0;
  // 90100 for OptiX 9.1.0.
  int optix_version = 0;
  // Why OptiX cannot be used, for GPUs that are kNoOptix.
  std::string optix_error;
};

// A loaded GPU module. Unloads it when destroyed.
class UMBRA_OWNER Module {
 public:
  // Loads the module at `path`. Fails with kNotFound when there is no file,
  // and with kUnavailable when it is not a GPU module of this build.
  [[nodiscard]] static std::expected<Module, core::Error> Load(
      const std::filesystem::path& path);

  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;
  Module(Module&& other) noexcept;
  Module& operator=(Module&& other) noexcept;
  ~Module();

  // Every GPU, usable or not. Fails with kUnavailable when the CUDA runtime
  // cannot be initialized.
  [[nodiscard]] std::expected<GpuDevices, core::Error> DescribeDevices() const;

  // A device on the usable GPU of CUDA ordinal `ordinal`, named
  // gpu<ordinal>, which commits into the GPU's memory and renders through
  // the module: the module must stay loaded while it lives. It renders a
  // product in batches of kTilesPerLaunch tiles, one launch each, and
  // copies each batch into the render buffers once it is rendered. Fails with
  // kUnavailable, and the cause, when the GPU cannot be used.
  [[nodiscard]] std::expected<std::unique_ptr<device::Device>, core::Error>
  CreateDevice(int ordinal) const UMBRA_LIFETIMEBOUND;

 private:
  Module(void* handle, const ModuleApi& api);

  void* UMBRA_NULLABLE handle_;
  const ModuleApi* UMBRA_NULLABLE api_;
};

// The GPUs of the module at `path`. Fails, with the reason, when the module
// is missing or the CUDA runtime cannot be initialized.
[[nodiscard]] std::expected<GpuDevices, core::Error> DescribeGpus(
    const std::filesystem::path& path);

}  // namespace umbra::device::gpu

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_GPU_MODULE_H_
