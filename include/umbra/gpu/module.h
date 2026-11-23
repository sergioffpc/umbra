// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Loads the GPU module, libumbra-gpu.so, at run time. umbra never links CUDA:
// the GPU backend lives in that module, which is loaded only when it is
// installed, and which registers GPU executors only when a usable GPU exists.

#ifndef UMBRA_GPU_MODULE_H_
#define UMBRA_GPU_MODULE_H_

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string_view>

#include "umbra/core/error.h"
#include "umbra/core/safety.h"
#include "umbra/gpu/module_abi.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::gpu {

// The file name of the GPU module.
inline constexpr std::string_view kModuleFileName = "libumbra-gpu.so";

// Where umbra looks for the GPU module: lib/libumbra-gpu.so next to the bin/
// directory of the running executable, in the build tree as when installed.
[[nodiscard]] std::expected<std::filesystem::path, core::Error>
DefaultModulePath();

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

  // The number of usable GPUs. Fails with kUnavailable when the driver or
  // OptiX cannot be initialized.
  [[nodiscard]] std::expected<std::int32_t, core::Error> CountDevices() const;

 private:
  Module(void* handle, const ModuleApi& api);

  void* UMBRA_NULLABLE handle_;
  const ModuleApi* UMBRA_NULLABLE api_;
};

// The number of GPU executors umbra can run: the usable GPUs of the module at
// `path`. Fails, with the reason, when there are none because the module is
// missing or the driver or OptiX cannot be initialized.
[[nodiscard]] std::expected<std::int32_t, core::Error> CountGpuExecutors(
    const std::filesystem::path& path);

}  // namespace umbra::gpu

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_GPU_MODULE_H_
