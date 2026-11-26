// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "accel.h"

#include <cuda_runtime_api.h>
#include <optix_host.h>
#include <optix_stubs.h>
#include <optix_types.h>

#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include "buffer.h"
#include "support.h"

namespace umbra::gpu::backend {
namespace {

// The memory a build or an update of `inputs` needs.
Result<OptixAccelBufferSizes> SizesOf(const Gpu& gpu,
                                      const OptixAccelBuildOptions& options,
                                      std::span<const OptixBuildInput> inputs) {
  OptixAccelBufferSizes sizes{};
  if (auto computed =
          CheckOptix(optixAccelComputeMemoryUsage(
                         gpu.context, &options, inputs.data(),
                         static_cast<unsigned>(inputs.size()), &sizes),
                     "cannot size an acceleration structure");
      !computed.has_value()) {
    return std::unexpected(computed.error());
  }
  return sizes;
}

// Runs a build or an update into `output`, and waits for it.
Result<OptixTraversableHandle> Run(const Gpu& gpu,
                                   const OptixAccelBuildOptions& options,
                                   std::span<const OptixBuildInput> inputs,
                                   const DeviceBuffer& temp,
                                   const DeviceBuffer& output) {
  OptixTraversableHandle handle = 0;
  if (auto built = CheckOptix(
          optixAccelBuild(gpu.context, gpu.stream, &options, inputs.data(),
                          static_cast<unsigned>(inputs.size()), temp.address(),
                          temp.size(), output.address(), output.size(), &handle,
                          nullptr, 0),
          "cannot build an acceleration structure");
      !built.has_value()) {
    return std::unexpected(built.error());
  }
  if (auto done = CheckCuda(cudaStreamSynchronize(gpu.stream),
                            "an acceleration structure build failed");
      !done.has_value()) {
    return std::unexpected(done.error());
  }
  return handle;
}

}  // namespace

Result<void> BuildAccel(const Gpu& gpu, const OptixAccelBuildOptions& options,
                        std::span<const OptixBuildInput> inputs,
                        AccelStructure& accel) {
  const auto sizes = SizesOf(gpu, options, inputs);
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

}  // namespace umbra::gpu::backend
