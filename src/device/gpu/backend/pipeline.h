// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The OptiX pipeline a GPU renders with: the placeholder integrator's
// programs (placeholder.cu), compiled from their PTX, and its shader
// binding table. It traces through motion transforms and deforming GASes,
// in a graph of any depth up to 3: IAS, motion transform, GAS.

#ifndef UMBRA_DEVICE_GPU_BACKEND_PIPELINE_H_
#define UMBRA_DEVICE_GPU_BACKEND_PIPELINE_H_

#include <driver_types.h>
#include <optix_types.h>

#include <array>
#include <cstdint>

#include "support.h"
#include "umbra/core/safety.h"
#include "umbra/ext/cuda/owners.h"
#include "umbra/ext/optix/owners.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device::gpu::backend {

// The program groups of the pipeline, in the order of its SBT: the ray
// generation, the miss, then the hit groups of triangles and of the
// lights' shapes, at their SBT offsets (accel.h).
inline constexpr std::size_t kProgramGroups = 4;

// The pipeline of a GPU and its SBT.
class Pipeline {
 public:
  // Compiles the programs on the GPU of `context` and uploads the SBT on
  // `stream`.
  [[nodiscard]] static Result<Pipeline> Create(OptixDeviceContext context,
                                               cudaStream_t stream);

  // Launches a grid of `width` × `height` × `depth` threads on `stream`,
  // with the LaunchParams at `params`, in the GPU's memory.
  [[nodiscard]] Result<void> Launch(CUdeviceptr params, std::uint32_t width,
                                    std::uint32_t height, std::uint32_t depth,
                                    cudaStream_t stream) const;

 private:
  Pipeline() = default;

  ext::optix::Module module_;
  std::array<ext::optix::ProgramGroup, kProgramGroups> groups_;
  ext::optix::Pipeline pipeline_;
  ext::cuda::DeviceBuffer records_;
  OptixShaderBindingTable sbt_{};
};

}  // namespace umbra::device::gpu::backend

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_GPU_BACKEND_PIPELINE_H_
