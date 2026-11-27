// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// OptiX's function table, which optixInit fills from the driver's OptiX
// runtime and every host call goes through.

#include "umbra/ext/optix/optix.h"

#include <optix.h>
#include <optix_function_table.h>
#include <optix_function_table_definition.h>  // IWYU pragma: keep
#include <optix_stubs.h>
#include <optix_types.h>

#include <expected>
#include <format>
#include <string>

#include "umbra/ext/optix/errors.h"

namespace umbra::ext::optix {

Result<void> Initialize() {
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

}  // namespace umbra::ext::optix
