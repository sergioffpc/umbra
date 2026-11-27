// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "accel.h"

#include <optix_types.h>

#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include "support.h"
#include "umbra/ext/cuda/owners.h"
#include "umbra/ext/cuda/runtime.h"
#include "umbra/ext/optix/host.h"

namespace umbra::device::gpu::backend {

using ext::cuda::DeviceBuffer;

namespace {

// Runs a build or an update into `output`, and waits for it.
Result<OptixTraversableHandle> Run(const Gpu& gpu,
                                   const OptixAccelBuildOptions& options,
                                   std::span<const OptixBuildInput> inputs,
                                   const DeviceBuffer& temp,
                                   const DeviceBuffer& output) {
  auto handle = ext::optix::AccelBuild(
      gpu.context, gpu.stream,
      {
          .options = options,
          .inputs = inputs,
          .temp = {.address = temp.address(), .size = temp.size()},
          .output = {.address = output.address(), .size = output.size()},
      },
      "cannot build an acceleration structure");
  if (!handle.has_value()) {
    return handle;
  }
  if (auto done = ext::cuda::Synchronize(
          gpu.stream, "an acceleration structure build failed");
      !done.has_value()) {
    return std::unexpected(done.error());
  }
  return handle;
}

}  // namespace

Result<void> BuildAccel(const Gpu& gpu, const OptixAccelBuildOptions& options,
                        std::span<const OptixBuildInput> inputs,
                        AccelStructure& accel) {
  const auto sizes = ext::optix::AccelMemoryUsage(
      gpu.context, options, inputs, "cannot size an acceleration structure");
  if (!sizes.has_value()) {
    return std::unexpected(sizes.error());
  }
  const bool updates = options.operation == OPTIX_BUILD_OPERATION_UPDATE;
  auto temp = DeviceBuffer::Allocate(updates ? sizes->tempUpdateSizeInBytes
                                             : sizes->tempSizeInBytes);
  if (!temp.has_value()) {
    return std::unexpected(temp.error());
  }
  if (updates) {
    return Run(gpu, options, inputs, *temp, accel.memory)
        .transform(
            [&](OptixTraversableHandle handle) { accel.handle = handle; });
  }
  auto output = DeviceBuffer::Allocate(sizes->outputSizeInBytes);
  if (!output.has_value()) {
    return std::unexpected(output.error());
  }
  return Run(gpu, options, inputs, *temp, *output)
      .transform([&](OptixTraversableHandle handle) {
        accel = {.memory = std::move(*output), .handle = handle};
      });
}

OptixAccelBuildOptions BuildOptions(unsigned flags,
                                    OptixBuildOperation operation,
                                    std::uint32_t keys) {
  return {
      .buildFlags = flags,
      .operation = operation,
      .motionOptions = MotionOptions(keys),
  };
}

OptixMotionOptions MotionOptions(std::uint32_t keys) {
  if (keys < 2) {
    return {
        .numKeys = 1,
        .flags = OPTIX_MOTION_FLAG_NONE,
        .timeBegin = 0.0F,
        .timeEnd = 0.0F,
    };
  }
  return {
      .numKeys = static_cast<std::uint16_t>(keys),
      .flags = OPTIX_MOTION_FLAG_NONE,
      .timeBegin = kMotionBegin,
      .timeEnd = kMotionEnd,
  };
}

}  // namespace umbra::device::gpu::backend
