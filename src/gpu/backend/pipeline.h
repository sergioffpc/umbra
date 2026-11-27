// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The OptiX pipeline a GPU renders with: the placeholder integrator's
// programs (placeholder.cu), compiled from their PTX, and its shader
// binding table. It traces through motion transforms and deforming GASes,
// in a graph of any depth up to 3: IAS, motion transform, GAS.

#ifndef UMBRA_GPU_BACKEND_PIPELINE_H_
#define UMBRA_GPU_BACKEND_PIPELINE_H_

#include <driver_types.h>
#include <optix_types.h>

#include <array>
#include <cstdint>
#include <memory>

#include "buffer.h"
#include "support.h"
#include "umbra/core/safety.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::gpu::backend {

// The OptiX objects a pipeline owns, each destroyed with it.
struct ModuleDestroyer {
  void operator()(OptixModule module) const;
};
struct ProgramGroupDestroyer {
  void operator()(OptixProgramGroup group) const;
};
struct PipelineDestroyer {
  void operator()(OptixPipeline pipeline) const;
};
using OwnedModule = std::unique_ptr<OptixModule_t, ModuleDestroyer>;
using OwnedProgramGroup =
    std::unique_ptr<OptixProgramGroup_t, ProgramGroupDestroyer>;
using OwnedPipeline = std::unique_ptr<OptixPipeline_t, PipelineDestroyer>;

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

  OwnedModule module_;
  std::array<OwnedProgramGroup, kProgramGroups> groups_;
  OwnedPipeline pipeline_;
  DeviceBuffer records_;
  OptixShaderBindingTable sbt_{};
};

}  // namespace umbra::gpu::backend

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_GPU_BACKEND_PIPELINE_H_
