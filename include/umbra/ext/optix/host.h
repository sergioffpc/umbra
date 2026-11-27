// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The calls into OptiX's host API that the GPU module makes, each failing
// with OptiX's message: what compiles programs into a pipeline and launches
// it, and what builds acceleration structures. The messages OptiX writes
// while it compiles are logged at debug level.

#ifndef UMBRA_EXT_OPTIX_HOST_H_
#define UMBRA_EXT_OPTIX_HOST_H_

#include <cuda.h>
#include <optix_types.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "umbra/core/safety.h"
#include "umbra/ext/optix/errors.h"
#include "umbra/ext/optix/owners.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::optix {

// Compiles the module of `input`, PTX or OptiX-IR, on `context`.
[[nodiscard]] Result<Module> CreateModule(
    OptixDeviceContext context, const OptixModuleCompileOptions& options,
    const OptixPipelineCompileOptions& pipeline, std::span<const char> input,
    std::string_view what);

// Creates the program groups `descriptions` describe on `context`, one into
// each of `groups`, which has as many: those it made even when it fails.
[[nodiscard]] Result<void> CreateProgramGroups(
    OptixDeviceContext context,
    std::span<const OptixProgramGroupDesc> descriptions,
    const OptixProgramGroupOptions& options, std::span<ProgramGroup> groups,
    std::string_view what);

// Links `groups` into a pipeline on `context`.
[[nodiscard]] Result<Pipeline> CreatePipeline(
    OptixDeviceContext context, const OptixPipelineCompileOptions& options,
    const OptixPipelineLinkOptions& link,
    std::span<const OptixProgramGroup> groups, std::string_view what);

// The call depths `pipeline`'s stacks are sized for.
struct CallDepths {
  unsigned trace = 0;
  unsigned continuation_callable = 0;
  unsigned direct_callable_from_state = 0;
  unsigned direct_callable_from_traversal = 0;
  unsigned traversable_graph = 0;
};

// Sizes the stacks of `pipeline` for `depths`.
[[nodiscard]] Result<void> SetStackSizes(OptixPipeline pipeline,
                                         const CallDepths& depths,
                                         std::string_view what);

// Writes the header of an SBT record of `group` into `header`, of
// OPTIX_SBT_RECORD_HEADER_SIZE bytes.
[[nodiscard]] Result<void> PackHeader(OptixProgramGroup group,
                                      std::span<char> header,
                                      std::string_view what);

// The size of a launch: a grid of `width` × `height` × `depth` threads.
struct LaunchSize {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t depth = 0;
};

// GPU memory: its address and its size in bytes.
struct DeviceMemory {
  CUdeviceptr address = 0;
  std::size_t size = 0;
};

// A launch of a pipeline: its launch parameters, in the GPU's memory, its
// SBT and its size.
struct LaunchRequest {
  DeviceMemory params;
  OptixShaderBindingTable sbt{};
  LaunchSize size;
};

// Queues on `stream` the launch of `pipeline` that `request` describes.
[[nodiscard]] Result<void> Launch(OptixPipeline pipeline, CUstream stream,
                                  const LaunchRequest& request,
                                  std::string_view what);

// The memory a build of `inputs` as `options` say needs.
[[nodiscard]] Result<OptixAccelBufferSizes> AccelMemoryUsage(
    OptixDeviceContext context, const OptixAccelBuildOptions& options,
    std::span<const OptixBuildInput> inputs, std::string_view what);

// A build of an acceleration structure: of `inputs`, as `options` say, with
// the temporary memory `temp`, into `output`.
struct AccelBuildRequest {
  OptixAccelBuildOptions options;
  std::span<const OptixBuildInput> inputs;
  DeviceMemory temp;
  DeviceMemory output;
};

// Queues on `stream` the build `request` describes, and tells its handle.
[[nodiscard]] Result<OptixTraversableHandle> AccelBuild(
    OptixDeviceContext context, CUstream stream,
    const AccelBuildRequest& request, std::string_view what);

// The handle of the traversable of `type` at `pointer`, in the GPU's memory.
[[nodiscard]] Result<OptixTraversableHandle> TraversableHandle(
    OptixDeviceContext context, CUdeviceptr pointer, OptixTraversableType type,
    std::string_view what);

}  // namespace umbra::ext::optix

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OPTIX_HOST_H_
