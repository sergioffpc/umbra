// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "device_listing.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "devices.h"
#include "umbra/core/cpu_topology.h"
#include "umbra/core/number_format.h"
#include "umbra/device/gpu/module.h"
#include "umbra/device/gpu/module_abi.h"

namespace umbra::devices {
namespace {

using ::umbra::core::CacheLevel;
using ::umbra::core::CacheType;
using ::umbra::core::CoreGroup;
using ::umbra::core::CpuNode;
using ::umbra::device::gpu::GpuDevice;
using ::umbra::device::gpu::GpuDevices;
using ::umbra::device::gpu::GpuUsability;

// The indent of a device's detail lines, the width of its name column.
constexpr std::string_view kIndent = "      ";

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

// The caches each core has to itself: "L1d 32.0 KiB, L2 512.0 KiB per core",
// or empty when there are none.
std::string PerCoreCaches(const CoreGroup& group) {
  std::string text;
  for (const CacheLevel& cache : group.caches) {
    if (cache.cores_per_instance == 1) {
      std::format_to(std::back_inserter(text), "{}{} {}",
                     text.empty() ? "" : ", ", CacheName(cache),
                     core::FormatBytes(cache.size_bytes));
    }
  }
  return text.empty() ? text : text + " per core";
}

// A cache shared by several cores: "L3 16.0 MiB per 4 cores, 128.0 MiB in 8
// slices", or "L3 32.0 MiB shared by 8 cores" when there is one.
std::string SharedCache(const CacheLevel& cache) {
  if (cache.instances == 1) {
    return std::format("{} {} shared by {} cores", CacheName(cache),
                       core::FormatBytes(cache.size_bytes),
                       cache.cores_per_instance);
  }
  return std::format(
      "{} {} per {} cores, {} in {} slices", CacheName(cache),
      core::FormatBytes(cache.size_bytes), cache.cores_per_instance,
      core::FormatBytes(cache.size_bytes *
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
// group, "8 cores: L2 1.0 MiB per core; L3 96.0 MiB shared by 8 cores".
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
                 NameColumn(DeviceName(DeviceKind::kCpu, node.node)),
                 node.model);
  std::format_to(std::back_inserter(text),
                 "{}NUMA node {}: {} cores, {} threads, {}\n", kIndent,
                 node.node, node.cores, node.threads,
                 core::FormatBytes(node.memory_bytes));
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
                 NameColumn(DeviceName(DeviceKind::kGpu, gpu.ordinal)),
                 gpu.name, gpu.sm_major, gpu.sm_minor);
  std::format_to(std::back_inserter(text), "{}{}", kIndent,
                 core::FormatBytes(gpu.memory_bytes));
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
std::string DeviceSummary(const Hardware& hardware,
                          const RenderDevice& device) {
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
             : std::format("{}, {}", gpu->name,
                           core::FormatBytes(gpu->memory_bytes));
}

}  // namespace

std::string FormatHardware(const Hardware& hardware) {
  std::string text;
  for (const CpuNode& node : hardware.cpus) {
    AppendCpu(node, hardware, text);
  }
  AppendGpus(hardware, text);
  return text;
}

std::string FormatRenderDevices(const Hardware& hardware,
                                std::span<const RenderDevice> chosen) {
  std::string text;
  for (const RenderDevice& device : chosen) {
    std::format_to(std::back_inserter(text), "  {}{}\n",
                   NameColumn(device.name), DeviceSummary(hardware, device));
  }
  return text;
}

}  // namespace umbra::devices
