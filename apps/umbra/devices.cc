// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "devices.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
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

using ::umbra::core::CacheLevel;
using ::umbra::core::CacheType;
using ::umbra::core::CoreGroup;
using ::umbra::core::CpuNode;
using ::umbra::core::Error;
using ::umbra::core::ErrorCode;
using ::umbra::device::gpu::GpuDevice;
using ::umbra::device::gpu::GpuDevices;
using ::umbra::device::gpu::GpuUsability;

// The indent of a device's detail lines, the width of its name column.
constexpr std::string_view kIndent = "      ";

constexpr std::array<std::string_view, 5> kByteUnits = {
    "B", "KiB", "MiB", "GiB", "TiB",
};

// A size in the largest binary unit it reaches: "512 KiB", "15.9 GiB".
std::string FormatBytes(std::uint64_t bytes) {
  constexpr std::uint64_t kUnit = 1024;
  std::uint64_t scale = 1;
  std::size_t unit = 0;
  // A size that rounds up to the next unit ("1024.0 MiB") is shown in it.
  constexpr std::uint64_t kRounding = 20;
  while (unit + 1 < kByteUnits.size() &&
         bytes >= (scale * kUnit) - (scale / kRounding)) {
    scale *= kUnit;
    ++unit;
  }
  if (bytes % scale == 0) {
    return std::format("{} {}", bytes / scale, kByteUnits.at(unit));
  }
  return std::format("{:.1f} {}",
                     static_cast<double>(bytes) / static_cast<double>(scale),
                     kByteUnits.at(unit));
}

// "L1d", "L1i", "L2".
std::string CacheName(const CacheLevel& cache) {
  switch (cache.type) {
    case CacheType::kData:
      return std::format("L{}d", cache.level);
    case CacheType::kInstruction:
      return std::format("L{}i", cache.level);
    case CacheType::kUnified:
      return std::format("L{}", cache.level);
  }
  return std::format("L{}", cache.level);
}

// The caches each core has to itself: "L1d 32 KiB, L2 512 KiB per core", or
// empty when there are none.
std::string PerCoreCaches(const CoreGroup& group) {
  std::string text;
  for (const CacheLevel& cache : group.caches) {
    if (cache.cores_per_instance == 1) {
      std::format_to(std::back_inserter(text), "{}{} {}",
                     text.empty() ? "" : ", ", CacheName(cache),
                     FormatBytes(cache.size_bytes));
    }
  }
  return text.empty() ? text : text + " per core";
}

// A cache shared by several cores: "L3 16 MiB per 4 cores, 128 MiB in 8
// slices", or "L3 32 MiB shared by 8 cores" when there is one.
std::string SharedCache(const CacheLevel& cache) {
  if (cache.instances == 1) {
    return std::format("{} {} shared by {} cores", CacheName(cache),
                       FormatBytes(cache.size_bytes), cache.cores_per_instance);
  }
  return std::format("{} {} per {} cores, {} in {} slices", CacheName(cache),
                     FormatBytes(cache.size_bytes), cache.cores_per_instance,
                     FormatBytes(cache.size_bytes *
                                 static_cast<std::uint64_t>(cache.instances)),
                     cache.instances);
}

std::vector<std::string> SharedCaches(const CoreGroup& group) {
  std::vector<std::string> lines;
  for (const CacheLevel& cache : group.caches) {
    if (cache.cores_per_instance > 1) {
      lines.push_back(SharedCache(cache));
    }
  }
  return lines;
}

// The cache lines of a node whose cores are all alike: the per-core caches
// with the line size, then one line per shared cache.
void AppendUniformCaches(const CpuNode& node, std::string& text) {
  const CoreGroup& group = node.core_groups.front();
  std::string per_core = PerCoreCaches(group);
  if (node.cache_line_bytes > 0) {
    std::format_to(std::back_inserter(per_core), "{}{} B lines",
                   per_core.empty() ? "" : ", ", node.cache_line_bytes);
  }
  if (!per_core.empty()) {
    std::format_to(std::back_inserter(text), "{}{}\n", kIndent, per_core);
  }
  for (const std::string& shared : SharedCaches(group)) {
    std::format_to(std::back_inserter(text), "{}{}\n", kIndent, shared);
  }
}

// The cache lines of a node with groups of different cores: one line per
// group, "8 cores: L2 1 MiB per core; L3 96 MiB shared by 8 cores".
void AppendGroupedCaches(const CpuNode& node, std::string& text) {
  for (const CoreGroup& group : node.core_groups) {
    std::string line = PerCoreCaches(group);
    for (const std::string& shared : SharedCaches(group)) {
      line += (line.empty() ? "" : "; ") + shared;
    }
    std::format_to(std::back_inserter(text), "{}{} cores: {}\n", kIndent,
                   group.cores, line);
  }
}

// The name column: the name and the spaces up to the detail indent.
std::string NameColumn(std::string_view name) {
  return std::format("{:<{}}", name, std::max(kIndent.size(), name.size() + 1));
}

void AppendCpu(const CpuNode& node, const Hardware& hardware,
               std::string& text) {
  std::format_to(std::back_inserter(text), "{}{}\n",
                 NameColumn(std::format("cpu{}", node.node)), node.model);
  std::format_to(std::back_inserter(text),
                 "{}NUMA node {}: {} cores, {} threads, {}\n", kIndent,
                 node.node, node.cores, node.threads,
                 FormatBytes(node.memory_bytes));
  if (node.core_groups.size() == 1) {
    AppendUniformCaches(node, text);
  } else {
    AppendGroupedCaches(node, text);
  }
  const std::string osl =
      hardware.osl_batch_width > 0
          ? std::format("OSL batched {}", hardware.osl_batch_width)
          : std::string("OSL scalar");
  std::format_to(std::back_inserter(text), "{}{}, Embree {}, {}\n", kIndent,
                 hardware.cpu_isa, hardware.embree_isa, osl);
}

// Why umbra cannot render on a GPU.
std::string UnusableReason(const GpuDevice& gpu, const GpuDevices& gpus) {
  switch (gpu.usability) {
    case GpuUsability::kTooOld:
      return "older than sm_120";
    case GpuUsability::kUnreadable:
      return "the CUDA runtime cannot describe it";
    case GpuUsability::kUsable:
    case GpuUsability::kNoOptix:
      break;
  }
  return gpus.optix_error;
}

// "13.4" for 13040.
std::string CudaVersion(int version) {
  constexpr int kMajor = 1000;
  constexpr int kMinor = 10;
  return std::format("{}.{}", version / kMajor, version % kMajor / kMinor);
}

// "9.1.0" for 90100.
std::string OptixVersion(int version) {
  constexpr int kMajor = 10000;
  constexpr int kMinor = 100;
  return std::format("{}.{}.{}", version / kMajor, version / kMinor % kMinor,
                     version % kMinor);
}

void AppendGpu(const GpuDevice& gpu, const GpuDevices& gpus,
               std::string& text) {
  if (gpu.usability == GpuUsability::kUnreadable) {
    std::format_to(std::back_inserter(text), "{}device {}: not usable, {}\n",
                   NameColumn("-"), gpu.ordinal, UnusableReason(gpu, gpus));
    return;
  }
  if (gpu.usability != GpuUsability::kUsable) {
    std::format_to(std::back_inserter(text), "{}{} (sm_{}{}): not usable, {}\n",
                   NameColumn("-"), gpu.name, gpu.sm_major, gpu.sm_minor,
                   UnusableReason(gpu, gpus));
    return;
  }
  std::format_to(std::back_inserter(text), "{}{} (sm_{}{})\n",
                 NameColumn(std::format("gpu{}", gpu.ordinal)), gpu.name,
                 gpu.sm_major, gpu.sm_minor);
  std::format_to(std::back_inserter(text), "{}{}", kIndent,
                 FormatBytes(gpu.memory_bytes));
  if (gpu.numa_node.has_value()) {
    std::format_to(std::back_inserter(text), ", NUMA node {}", *gpu.numa_node);
  }
  const std::string driver =
      gpus.driver_version.empty()
          ? std::string()
          : std::format("driver {}, ", gpus.driver_version);
  std::format_to(std::back_inserter(text), "\n{}{}CUDA {}, OptiX {}\n", kIndent,
                 driver, CudaVersion(gpus.cuda_driver_version),
                 OptixVersion(gpus.optix_version));
}

void AppendGpus(const Hardware& hardware, std::string& text) {
  if (!hardware.gpus.has_value()) {
    std::format_to(std::back_inserter(text), "{}none: {}\n", NameColumn("gpu"),
                   hardware.gpus.error().message());
    return;
  }
  if (hardware.gpus->gpus.empty()) {
    std::format_to(std::back_inserter(text), "{}none\n", NameColumn("gpu"));
    return;
  }
  for (const GpuDevice& gpu : hardware.gpus->gpus) {
    AppendGpu(gpu, *hardware.gpus, text);
  }
}

// What a render start block says of a device: the model and threads of a
// CPU node, the name and memory of a GPU.
std::string DeviceSummary(const Hardware& hardware, const Device& device) {
  if (device.kind == DeviceKind::kCpu) {
    const auto node =
        std::ranges::find(hardware.cpus, device.index, &CpuNode::node);
    return node == hardware.cpus.end()
               ? std::string()
               : std::format("{}, {} threads", node->model, node->threads);
  }
  if (!hardware.gpus.has_value()) {
    return {};
  }
  const auto gpu =
      std::ranges::find(hardware.gpus->gpus, device.index, &GpuDevice::ordinal);
  return gpu == hardware.gpus->gpus.end()
             ? std::string()
             : std::format("{}, {}", gpu->name, FormatBytes(gpu->memory_bytes));
}

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
    const Device& device,
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

}  // namespace

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

std::vector<Device> RenderDevices(const Hardware& hardware) {
  std::vector<Device> devices;
  devices.reserve(hardware.cpus.size() +
                  (hardware.gpus.has_value() ? hardware.gpus->gpus.size() : 0));
  for (const CpuNode& node : hardware.cpus) {
    devices.push_back({
        .kind = DeviceKind::kCpu,
        .index = node.node,
        .name = std::format("cpu{}", node.node),
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
          .name = std::format("gpu{}", gpu.ordinal),
      });
    }
  }
  return devices;
}

std::string FormatHardware(const Hardware& hardware) {
  std::string text;
  for (const CpuNode& node : hardware.cpus) {
    AppendCpu(node, hardware, text);
  }
  AppendGpus(hardware, text);
  return text;
}

std::string FormatRenderDevices(const Hardware& hardware,
                                std::span<const Device> chosen) {
  std::string text;
  for (const Device& device : chosen) {
    std::format_to(std::back_inserter(text), "  {}{}\n",
                   NameColumn(device.name), DeviceSummary(hardware, device));
  }
  return text;
}

std::expected<std::vector<Device>, Error> ResolveDevices(
    std::string_view spec, std::span<const Device> devices) {
  if (spec.empty()) {
    return std::vector<Device>(devices.begin(), devices.end());
  }
  std::vector<Device> chosen;
  for (std::size_t start = 0; start <= spec.size();) {
    const std::size_t comma = std::min(spec.find(',', start), spec.size());
    const std::string_view name = spec.substr(start, comma - start);
    start = comma + 1;
    if (name.empty()) {
      return std::unexpected(DeviceError(
          std::format("--devices={}: a device name is empty", spec)));
    }
    const auto found = std::ranges::find(devices, name, &Device::name);
    if (found == devices.end()) {
      return std::unexpected(DeviceError(std::format(
          "unknown device '{}'; run umbra devices to list them", name)));
    }
    if (std::ranges::contains(chosen, name, &Device::name)) {
      return std::unexpected(
          DeviceError(std::format("device '{}' is given twice", name)));
    }
    chosen.push_back(*found);
  }
  return chosen;
}

std::expected<CreatedDevices, Error> CreateDevices(
    std::span<const Device> chosen) {
  CreatedDevices created;
  for (const Device& device : chosen) {
    auto made = Create(device, created.module);
    if (!made.has_value()) {
      return std::unexpected(made.error());
    }
    created.devices.push_back(*std::move(made));
  }
  if (core::IsLogEnabled(core::LogComponent::kDevice, core::LogLevel::kDebug)) {
    created.devices.push_back(std::make_unique<device::DiagnosticDevice>());
  }
  return created;
}

}  // namespace umbra::devices
