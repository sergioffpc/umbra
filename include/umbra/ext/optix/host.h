// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// The calls into OptiX's host API that the GPU module makes, each failing
// with OptiX's message: what compiles programs into a pipeline and launches
// it, and what builds acceleration structures. A call that succeeds formats
// no message. The messages OptiX writes while it compiles are logged at
// debug level.

#ifndef UMBRA_EXT_OPTIX_HOST_H_
#define UMBRA_EXT_OPTIX_HOST_H_

#include <cuda.h>
#include <optix_types.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <span>

#include "umbra/core/lazy_format.h"
#include "umbra/core/safety.h"
#include "umbra/ext/optix/errors.h"
#include "umbra/ext/optix/owners.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::ext::optix {

// Each call fails with OptiX's message, "OptiX: <what>: <message> (<name>)",
// and takes `what` either made (core::LazyFormat) or as a format string and
// its arguments, which it formats only when it fails.

// Compiles the module of `input`, PTX or OptiX-IR, on `context`.
[[nodiscard]] Result<Module> CreateModule(
    OptixDeviceContext context, const OptixModuleCompileOptions& options,
    const OptixPipelineCompileOptions& pipeline, std::span<const char> input,
    const core::LazyFormat& what);

template <typename... Args>
// NOLINTNEXTLINE(readability-function-size): OptiX's and the format's args.
[[nodiscard]] Result<Module> CreateModule(
    OptixDeviceContext context, const OptixModuleCompileOptions& options,
    const OptixPipelineCompileOptions& pipeline, std::span<const char> input,
    std::format_string<Args...> what, const Args&... args) {
  return CreateModule(
      context, options, pipeline, input,
      core::LazyFormat(what.get(), std::make_format_args(args...)));
}

// Creates the program groups `descriptions` describe on `context`, one into
// each of `groups`, which has as many: those it made even when it fails.
[[nodiscard]] Result<void> CreateProgramGroups(
    OptixDeviceContext context,
    std::span<const OptixProgramGroupDesc> descriptions,
    const OptixProgramGroupOptions& options, std::span<ProgramGroup> groups,
    const core::LazyFormat& what);

template <typename... Args>
// NOLINTNEXTLINE(readability-function-size): OptiX's and the format's args.
[[nodiscard]] Result<void> CreateProgramGroups(
    OptixDeviceContext context,
    std::span<const OptixProgramGroupDesc> descriptions,
    const OptixProgramGroupOptions& options, std::span<ProgramGroup> groups,
    std::format_string<Args...> what, const Args&... args) {
  return CreateProgramGroups(
      context, descriptions, options, groups,
      core::LazyFormat(what.get(), std::make_format_args(args...)));
}

// Links `groups` into a pipeline on `context`.
[[nodiscard]] Result<Pipeline> CreatePipeline(
    OptixDeviceContext context, const OptixPipelineCompileOptions& options,
    const OptixPipelineLinkOptions& link,
    std::span<const OptixProgramGroup> groups, const core::LazyFormat& what);

template <typename... Args>
// NOLINTNEXTLINE(readability-function-size): OptiX's and the format's args.
[[nodiscard]] Result<Pipeline> CreatePipeline(
    OptixDeviceContext context, const OptixPipelineCompileOptions& options,
    const OptixPipelineLinkOptions& link,
    std::span<const OptixProgramGroup> groups, std::format_string<Args...> what,
    const Args&... args) {
  return CreatePipeline(
      context, options, link, groups,
      core::LazyFormat(what.get(), std::make_format_args(args...)));
}

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
                                         const core::LazyFormat& what);

template <typename... Args>
[[nodiscard]] Result<void> SetStackSizes(OptixPipeline pipeline,
                                         const CallDepths& depths,
                                         std::format_string<Args...> what,
                                         const Args&... args) {
  return SetStackSizes(
      pipeline, depths,
      core::LazyFormat(what.get(), std::make_format_args(args...)));
}

// Writes the header of an SBT record of `group` into `header`, of
// OPTIX_SBT_RECORD_HEADER_SIZE bytes.
[[nodiscard]] Result<void> PackHeader(OptixProgramGroup group,
                                      std::span<char> header,
                                      const core::LazyFormat& what);

template <typename... Args>
[[nodiscard]] Result<void> PackHeader(OptixProgramGroup group,
                                      std::span<char> header,
                                      std::format_string<Args...> what,
                                      const Args&... args) {
  return PackHeader(
      group, header,
      core::LazyFormat(what.get(), std::make_format_args(args...)));
}

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
                                  const core::LazyFormat& what);

template <typename... Args>
[[nodiscard]] Result<void> Launch(OptixPipeline pipeline, CUstream stream,
                                  const LaunchRequest& request,
                                  std::format_string<Args...> what,
                                  const Args&... args) {
  return Launch(pipeline, stream, request,
                core::LazyFormat(what.get(), std::make_format_args(args...)));
}

// The memory a build of `inputs` as `options` say needs.
[[nodiscard]] Result<OptixAccelBufferSizes> AccelMemoryUsage(
    OptixDeviceContext context, const OptixAccelBuildOptions& options,
    std::span<const OptixBuildInput> inputs, const core::LazyFormat& what);

template <typename... Args>
[[nodiscard]] Result<OptixAccelBufferSizes> AccelMemoryUsage(
    OptixDeviceContext context, const OptixAccelBuildOptions& options,
    std::span<const OptixBuildInput> inputs, std::format_string<Args...> what,
    const Args&... args) {
  return AccelMemoryUsage(
      context, options, inputs,
      core::LazyFormat(what.get(), std::make_format_args(args...)));
}

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
    const AccelBuildRequest& request, const core::LazyFormat& what);

template <typename... Args>
[[nodiscard]] Result<OptixTraversableHandle> AccelBuild(
    OptixDeviceContext context, CUstream stream,
    const AccelBuildRequest& request, std::format_string<Args...> what,
    const Args&... args) {
  return AccelBuild(
      context, stream, request,
      core::LazyFormat(what.get(), std::make_format_args(args...)));
}

// The handle of the traversable of `type` at `pointer`, in the GPU's memory.
[[nodiscard]] Result<OptixTraversableHandle> TraversableHandle(
    OptixDeviceContext context, CUdeviceptr pointer, OptixTraversableType type,
    const core::LazyFormat& what);

template <typename... Args>
[[nodiscard]] Result<OptixTraversableHandle> TraversableHandle(
    OptixDeviceContext context, CUdeviceptr pointer, OptixTraversableType type,
    std::format_string<Args...> what, const Args&... args) {
  return TraversableHandle(
      context, pointer, type,
      core::LazyFormat(what.get(), std::make_format_args(args...)));
}

}  // namespace umbra::ext::optix

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_EXT_OPTIX_HOST_H_
