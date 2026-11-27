// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// OptiX acceleration structures, as a GPU holds them, and how the GPU module
// builds them.

#ifndef UMBRA_DEVICE_GPU_BACKEND_ACCEL_H_
#define UMBRA_DEVICE_GPU_BACKEND_ACCEL_H_

#include <driver_types.h>
#include <optix_types.h>

#include <cstdint>
#include <span>
#include <string_view>

#include "support.h"
#include "umbra/core/safety.h"
#include "umbra/ext/cuda/owners.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device::gpu::backend {

// The GPU a device works on, as its builds need it.
struct Gpu {
  OptixDeviceContext context;
  cudaStream_t stream;
  // "gpu0", for the messages.
  std::string_view name;
};

// The visibility masks of the instances of the IAS.
inline constexpr std::uint32_t kGeometryMask = 1U << 0U;
inline constexpr std::uint32_t kLightMask = 1U << 1U;

// The SBT offsets of the instances of the IAS: the hit group of triangles,
// then that of the analytic lights.
inline constexpr std::uint32_t kTriangleHitGroup = 0;
inline constexpr std::uint32_t kLightHitGroup = 1;

// The time at which motion begins and ends, the shutter's open and close:
// the motion keys of a GAS or of a motion transform span it.
inline constexpr float kMotionBegin = 0.0F;
inline constexpr float kMotionEnd = 1.0F;

// An acceleration structure in a GPU's memory; none when its handle is 0.
struct AccelStructure {
  ext::cuda::DeviceBuffer memory;
  OptixTraversableHandle handle = 0;
};

// Builds `accel` from `inputs`, as `options` say: anew, into memory of its
// own, or as an update of what it holds, in place. Waits for the build, so
// that the inputs may be freed when it returns.
[[nodiscard]] Result<void> BuildAccel(const Gpu& gpu,
                                      const OptixAccelBuildOptions& options,
                                      std::span<const OptixBuildInput> inputs,
                                      AccelStructure& accel);

// The options of a build of `flags` (OptixBuildFlags), as `operation`, with
// `keys` motion keys.
[[nodiscard]] OptixAccelBuildOptions BuildOptions(unsigned flags,
                                                  OptixBuildOperation operation,
                                                  std::uint32_t keys);

// The motion options of `keys` motion keys over the shutter.
[[nodiscard]] OptixMotionOptions MotionOptions(std::uint32_t keys);

}  // namespace umbra::device::gpu::backend

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_GPU_BACKEND_ACCEL_H_
