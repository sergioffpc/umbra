// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "devices.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "umbra/core/cpu_topology.h"
#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/core/safety.h"
#include "umbra/device/cpu/cpu_device.h"
#include "umbra/device/device.h"
#include "umbra/device/diagnostic_device.h"
#include "umbra/device/gpu/module.h"
#include "umbra/device/gpu/module_abi.h"
#include "umbra/ext/embree/embree.h"
#include "umbra/ext/osl/osl.h"

namespace umbra::devices {
namespace {

using ::umbra::core::CpuNode;
using ::umbra::core::Error;
using ::umbra::core::ErrorCode;
using ::umbra::device::gpu::GpuDevice;
using ::umbra::device::gpu::GpuUsability;

Error DeviceError(std::string message) {
  return {ErrorCode::kInvalidArgument, std::move(message)};
}

// The GPU module `module` holds, loaded into it unless it holds one.
std::expected<device::gpu::Module*, Error> LoadOnce(
    std::optional<device::gpu::Module>& module UMBRA_LIFETIMEBOUND) {
  if (module.has_value()) {
    return &*module;
  }
  return device::gpu::DefaultModulePath()
      .and_then(device::gpu::Module::Load)
      .transform([&](device::gpu::Module loaded) {
        return &module.emplace(std::move(loaded));
      });
}

// The device `device` names: a CPU device on its node, or a GPU device
// made through the GPU module, which `module` holds once loaded. Fails with
// the cause, naming the device, when the module cannot be loaded or the GPU
// cannot be used.
std::expected<std::unique_ptr<device::Device>, Error> Create(
    const RenderDevice& device,
    std::optional<device::gpu::Module>& module UMBRA_LIFETIMEBOUND) {
  if (device.kind == DeviceKind::kCpu) {
    return std::make_unique<device::cpu::CpuDevice>(device.index);
  }
  return LoadOnce(module)
      .and_then([&](device::gpu::Module* loaded) {
        return loaded->CreateDevice(device.index);
      })
      .transform_error([&](const Error& error) {
        return Error(error.code(), std::format("device {}: {}", device.name,
                                               error.message()));
      });
}

// Adds `device` to the devices of `created`, which owns it.
void Own(std::unique_ptr<device::Device> device, CreatedDevices& created) {
  created.devices.push_back(device.get());
  created.owned.push_back(std::move(device));
}

}  // namespace

std::string DeviceName(DeviceKind kind, int index) {
  return std::format("{}{}", kind == DeviceKind::kCpu ? "cpu" : "gpu", index);
}

std::expected<Hardware, Error> ReadHardware() {
  auto cpus = core::ReadCpuNodes();
  if (!cpus.has_value()) {
    return std::unexpected(cpus.error());
  }
  return Hardware{
      .cpus = *std::move(cpus),
      .gpus =
          device::gpu::DefaultModulePath().and_then(device::gpu::DescribeGpus),
      .cpu_isa = core::CpuVectorIsa(),
      .embree_isa = ext::embree::NativeIsa(),
      .osl_batch_width = ext::osl::BatchWidth(),
  };
}

std::vector<RenderDevice> RenderDevices(const Hardware& hardware) {
  std::vector<RenderDevice> devices;
  devices.reserve(hardware.cpus.size() +
                  (hardware.gpus.has_value() ? hardware.gpus->gpus.size() : 0));
  for (const CpuNode& node : hardware.cpus) {
    devices.push_back({
        .kind = DeviceKind::kCpu,
        .index = node.node,
        .name = DeviceName(DeviceKind::kCpu, node.node),
    });
  }
  if (!hardware.gpus.has_value()) {
    return devices;
  }
  for (const GpuDevice& gpu : hardware.gpus->gpus) {
    if (gpu.usability == GpuUsability::kUsable) {
      devices.push_back({
          .kind = DeviceKind::kGpu,
          .index = gpu.ordinal,
          .name = DeviceName(DeviceKind::kGpu, gpu.ordinal),
      });
    }
  }
  return devices;
}

std::expected<std::vector<RenderDevice>, Error> ResolveDevices(
    std::string_view spec, std::span<const RenderDevice> devices) {
  if (spec.empty()) {
    return std::vector<RenderDevice>(devices.begin(), devices.end());
  }
  std::vector<RenderDevice> chosen;
  for (std::size_t start = 0; start <= spec.size();) {
    const std::size_t comma = std::min(spec.find(',', start), spec.size());
    const std::string_view name = spec.substr(start, comma - start);
    start = comma + 1;
    if (name.empty()) {
      return std::unexpected(DeviceError(
          std::format("--devices={}: a device name is empty", spec)));
    }
    const auto found = std::ranges::find(devices, name, &RenderDevice::name);
    if (found == devices.end()) {
      return std::unexpected(DeviceError(std::format(
          "unknown device '{}'; run umbra devices to list them", name)));
    }
    if (std::ranges::contains(chosen, name, &RenderDevice::name)) {
      return std::unexpected(
          DeviceError(std::format("device '{}' is given twice", name)));
    }
    chosen.push_back(*found);
  }
  return chosen;
}

std::expected<CreatedDevices, Error> CreateDevices(
    std::span<const RenderDevice> chosen) {
  CreatedDevices created;
  for (const RenderDevice& device : chosen) {
    auto made = Create(device, created.module);
    if (!made.has_value()) {
      return std::unexpected(made.error());
    }
    Own(*std::move(made), created);
    if (device.kind == DeviceKind::kGpu) {
      created.gpus.push_back(
          {.cuda_ordinal = device.index, .name = device.name});
    }
  }
  if (core::IsLogEnabled(core::LogComponent::kDevice, core::LogLevel::kDebug)) {
    Own(std::make_unique<device::DiagnosticDevice>(), created);
  }
  return created;
}

}  // namespace umbra::devices
