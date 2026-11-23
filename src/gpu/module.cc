// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/gpu/module.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <system_error>
#include <utility>

#include "umbra/core/error.h"
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

std::expected<std::int32_t, Error> Module::CountDevices() const {
  std::int32_t count = 0;
  return ResultOf(api_->count_devices(&count)).transform([count] {
    return count;
  });
}

std::expected<std::int32_t, Error> CountGpuExecutors(
    const std::filesystem::path& path) {
  return Module::Load(path).and_then(
      [](const Module& module) { return module.CountDevices(); });
}

}  // namespace umbra::gpu
