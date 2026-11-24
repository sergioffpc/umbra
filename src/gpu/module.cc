// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/gpu/module.h"

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "umbra/core/error.h"
#include "umbra/core/log.h"
#include "umbra/gpu/module_abi.h"

namespace umbra::gpu {
namespace {

using ::umbra::core::Error;
using ::umbra::core::ErrorCode;

// The last dlopen or dlsym error.
std::string DlError() {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): glibc keeps it per thread.
  const char* error = dlerror();
  return error == nullptr ? std::string("unknown error") : std::string(error);
}

// The module's diagnostics, under the gpu component.
void LogFromModule(core::LogLevel level, std::string_view message) {
  core::Log(level, core::LogComponent::kGpu, message);
}

// The lowest level of the gpu component that is shown.
core::LogLevel GpuThreshold() {
  constexpr std::array kLevels = {
      core::LogLevel::kTrace,
      core::LogLevel::kDebug,
      core::LogLevel::kInfo,
      core::LogLevel::kWarning,
  };
  const auto* const shown = std::ranges::find_if(kLevels, [](auto level) {
    return core::IsLogEnabled(core::LogComponent::kGpu, level);
  });
  return shown == kLevels.end() ? core::LogLevel::kError : *shown;
}

// A module status as the result of a loader call: ok, or kUnavailable with
// the status's message, up to its terminating NUL.
std::expected<void, Error> ResultOf(const ModuleStatus& status) {
  if (status.outcome == ModuleOutcome::kOk) {
    return {};
  }
  const auto* const end = std::ranges::find(status.message, '\0');
  return std::unexpected(
      Error(ErrorCode::kUnavailable, std::string(status.message.begin(), end)));
}

// The first line of a file, or empty when it cannot be read.
std::string FirstLine(const std::filesystem::path& path) {
  std::ifstream file(path);
  std::string line;
  std::getline(file, line);
  return line;
}

// The NUMA node of the PCI device at `bus_id` ("0000:01:00.0"), from sysfs,
// which spells it in lowercase with a four-digit domain.
std::optional<int> NumaNodeOf(std::string bus_id) {
  if (bus_id.empty()) {
    return std::nullopt;
  }
  std::ranges::transform(bus_id, bus_id.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  constexpr std::size_t kSysfsLength = 12;
  if (bus_id.size() > kSysfsLength) {
    bus_id.erase(0, bus_id.size() - kSysfsLength);
  }
  const std::string node = FirstLine(
      std::filesystem::path("/sys/bus/pci/devices") / bus_id / "numa_node");
  int value = -1;
  std::from_chars(std::to_address(node.begin()), std::to_address(node.end()),
                  value);
  return value < 0 ? std::nullopt : std::optional<int>(value);
}

GpuDevice DeviceOf(const GpuDescription& gpu) {
  return {
      .ordinal = gpu.ordinal,
      .usability = gpu.usability,
      .name = std::string(TextOf(gpu.name)),
      .sm_major = gpu.sm_major,
      .sm_minor = gpu.sm_minor,
      .memory_bytes = gpu.memory_bytes,
      .numa_node = NumaNodeOf(std::string(TextOf(gpu.pci_bus_id))),
  };
}

}  // namespace

std::expected<std::filesystem::path, Error> DefaultModulePath() {
  std::error_code error;
  const std::filesystem::path executable =
      std::filesystem::read_symlink("/proc/self/exe", error);
  if (error) {
    return std::unexpected(Error(
        ErrorCode::kUnavailable,
        std::format("cannot locate the executable: {}", error.message())));
  }
  return executable.parent_path().parent_path() / "lib" / kModuleFileName;
}

std::expected<Module, Error> Module::Load(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    return std::unexpected(
        Error(ErrorCode::kNotFound,
              std::format("{} is not installed", path.string())));
  }
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    return std::unexpected(
        Error(ErrorCode::kUnavailable,
              std::format("cannot load {}: {}", path.string(), DlError())));
  }
  void* symbol = dlsym(handle, kModuleEntryPoint);
  if (symbol == nullptr) {
    dlclose(handle);
    return std::unexpected(
        Error(ErrorCode::kUnavailable,
              std::format("{} has no entry point {}", path.string(),
                          kModuleEntryPoint)));
  }
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): dlsym API.
  const auto entry_point = reinterpret_cast<ModuleEntryPoint>(symbol);
  const ModuleApi* api = entry_point();
  // Read before dlclose, which unmaps the table.
  const std::uint32_t abi_version = api == nullptr ? 0 : api->abi_version;
  if (abi_version != kModuleAbiVersion) {
    dlclose(handle);
    return std::unexpected(
        Error(ErrorCode::kUnavailable,
              std::format("{} has ABI version {}, umbra needs {}",
                          path.string(), abi_version, kModuleAbiVersion)));
  }
  api->set_log(LogFromModule, GpuThreshold());
  return Module(handle, *api);
}

Module::Module(void* handle, const ModuleApi& api)
    : handle_(handle), api_(&api) {}

Module::Module(Module&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      api_(std::exchange(other.api_, nullptr)) {}

Module& Module::operator=(Module&& other) noexcept {
  if (this != &other) {
    if (handle_ != nullptr) {
      dlclose(handle_);
    }
    handle_ = std::exchange(other.handle_, nullptr);
    api_ = std::exchange(other.api_, nullptr);
  }
  return *this;
}

Module::~Module() {
  if (handle_ != nullptr) {
    dlclose(handle_);
  }
}


std::expected<GpuDevices, Error> Module::DescribeDevices() const {
  const auto inventory = std::make_unique<GpuInventory>();
  if (auto described = ResultOf(api_->describe_devices(inventory.get()));
      !described.has_value()) {
    return std::unexpected(described.error());
  }
  GpuDevices devices{
      .gpus = {},
      .driver_version = FirstLine("/sys/module/nvidia/version"),
      .cuda_driver_version = inventory->cuda_driver_version,
      .optix_version = inventory->optix_version,
      .optix_error = std::string(TextOf(inventory->optix_error)),
  };
  const auto described = DescribedGpus(*inventory);
  devices.gpus.reserve(described.size());
  for (const GpuDescription& gpu : described) {
    devices.gpus.push_back(DeviceOf(gpu));
  }
  return devices;
}

std::expected<GpuDevices, Error> DescribeGpus(
    const std::filesystem::path& path) {
  return Module::Load(path).and_then(
      [](const Module& module) { return module.DescribeDevices(); });
}


}  // namespace umbra::gpu
