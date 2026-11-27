// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "support.h"

#include <cuda_runtime_api.h>
#include <driver_types.h>
#include <optix.h>
#include <optix_function_table.h>
#include <optix_stubs.h>
#include <optix_types.h>

#include <expected>
#include <format>
#include <string>
#include <string_view>

namespace umbra::gpu::backend {

LogState& Logging() {
  static LogState state;
  return state;
}

Result<void> CheckCuda(cudaError_t error, std::string_view what) {
  if (error == cudaSuccess) {
    return {};
  }
  return std::unexpected(std::format("CUDA: {}: {} ({})", what,
                                     cudaGetErrorString(error),
                                     cudaGetErrorName(error)));
}

Result<void> CheckOptix(OptixResult result, std::string_view what) {
  if (result == OPTIX_SUCCESS) {
    return {};
  }
  return std::unexpected(std::format("OptiX: {}: {} ({})", what,
                                     optixGetErrorString(result),
                                     optixGetErrorName(result)));
}

Result<void> InitOptix() {
  static const OptixResult kResult = optixInit();
  switch (kResult) {
    case OPTIX_SUCCESS:
      return {};
    case OPTIX_ERROR_LIBRARY_NOT_FOUND:
      return std::unexpected(std::string(
          "OptiX: the driver's OptiX runtime (libnvoptix.so.1) is missing; "
          "in a container, the GPU must come with the graphics libraries"));
    case OPTIX_ERROR_UNSUPPORTED_ABI_VERSION:
      return std::unexpected(std::format(
          "OptiX: the driver is too old for OptiX {}.{}.{} (ABI {})",
          OPTIX_VERSION / 10000, OPTIX_VERSION / 100 % 100, OPTIX_VERSION % 100,
          OPTIX_ABI_VERSION));
    default:
      return std::unexpected(std::format("OptiX: optixInit failed ({})",
                                         static_cast<int>(kResult)));
  }
}

}  // namespace umbra::gpu::backend
