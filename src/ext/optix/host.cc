// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/optix/host.h"

#include <cuda.h>
#include <optix_host.h>
#include <optix_stubs.h>
#include <optix_types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "log.h"
#include "umbra/core/lazy_format.h"
#include "umbra/core/log.h"
#include "umbra/core/log_hook.h"
#include "umbra/ext/optix/errors.h"
#include "umbra/ext/optix/owners.h"

namespace umbra::ext::optix {
namespace {

// The room OptiX's messages are written into.
using MessageBuffer = std::array<char, 2048>;

// The message OptiX wrote into `buffer`, `size` bytes with its NUL.
std::string MessageOf(const MessageBuffer& buffer, std::size_t size) {
  const std::size_t length = size == 0 ? 0 : std::min(size, buffer.size()) - 1;
  return {buffer.data(), length};
}

// Logs at debug level the message of an OptiX call, if any.
void LogMessage(std::string_view what, const MessageBuffer& buffer,
                std::size_t size) {
  if (std::string message = MessageOf(buffer, size); !message.empty()) {
    Logging().Log(core::LogLevel::kDebug,
                  std::format("OptiX: {}: {}", what, message));
  }
}

}  // namespace

Result<Module> CreateModule(OptixDeviceContext context,
                            const OptixModuleCompileOptions& options,
                            const OptixPipelineCompileOptions& pipeline,
                            std::span<const char> input,
                            const core::LazyFormat& what) {
  MessageBuffer message{};
  std::size_t size = message.size();
  OptixModule module = nullptr;
  auto created =
      Check(optixModuleCreate(context, &options, &pipeline, input.data(),
                              input.size(), message.data(), &size, &module),
            what);
  LogMessage("module", message, size);
  return created.transform([&] { return Module(module); });
}

Result<void> CreateProgramGroups(
    OptixDeviceContext context,
    std::span<const OptixProgramGroupDesc> descriptions,
    const OptixProgramGroupOptions& options, std::span<ProgramGroup> groups,
    const core::LazyFormat& what) {
  std::vector<OptixProgramGroup> created(descriptions.size(), nullptr);
  MessageBuffer message{};
  std::size_t size = message.size();
  auto result = Check(
      optixProgramGroupCreate(context, descriptions.data(),
                              static_cast<unsigned>(descriptions.size()),
                              &options, message.data(), &size, created.data()),
      what);
  LogMessage("program groups", message, size);
  for (auto [group, into] : std::views::zip(created, groups)) {
    into.reset(group);
  }
  return result;
}

Result<Pipeline> CreatePipeline(OptixDeviceContext context,
                                const OptixPipelineCompileOptions& options,
                                const OptixPipelineLinkOptions& link,
                                std::span<const OptixProgramGroup> groups,
                                const core::LazyFormat& what) {
  MessageBuffer message{};
  std::size_t size = message.size();
  OptixPipeline pipeline = nullptr;
  auto created =
      Check(optixPipelineCreate(context, &options, &link, groups.data(),
                                static_cast<unsigned>(groups.size()),
                                message.data(), &size, &pipeline),
            what);
  LogMessage("pipeline", message, size);
  Pipeline owned(pipeline);
  return created.transform([&] { return std::move(owned); });
}

Result<void> SetStackSizes(OptixPipeline pipeline, const CallDepths& depths,
                           const core::LazyFormat& what) {
  return Check(
      optixPipelineSetStackSizeFromCallDepths(
          pipeline, depths.trace, depths.continuation_callable,
          depths.direct_callable_from_state,
          depths.direct_callable_from_traversal, depths.traversable_graph),
      what);
}

Result<void> PackHeader(OptixProgramGroup group, std::span<char> header,
                        const core::LazyFormat& what) {
  if (header.size() < OPTIX_SBT_RECORD_HEADER_SIZE) {
    return std::unexpected(std::format(
        "OptiX: {}: a header of {} bytes is smaller than {}", what.Format(),
        header.size(), OPTIX_SBT_RECORD_HEADER_SIZE));
  }
  return Check(optixSbtRecordPackHeader(group, header.data()), what);
}

Result<void> Launch(OptixPipeline pipeline, CUstream stream,
                    const LaunchRequest& request,
                    const core::LazyFormat& what) {
  return Check(
      optixLaunch(pipeline, stream, request.params.address, request.params.size,
                  &request.sbt, request.size.width, request.size.height,
                  request.size.depth),
      what);
}

Result<OptixAccelBufferSizes> AccelMemoryUsage(
    OptixDeviceContext context, const OptixAccelBuildOptions& options,
    std::span<const OptixBuildInput> inputs, const core::LazyFormat& what) {
  OptixAccelBufferSizes sizes{};
  return Check(optixAccelComputeMemoryUsage(
                   context, &options, inputs.data(),
                   static_cast<unsigned>(inputs.size()), &sizes),
               what)
      .transform([&] { return sizes; });
}

Result<OptixTraversableHandle> AccelBuild(OptixDeviceContext context,
                                          CUstream stream,
                                          const AccelBuildRequest& request,
                                          const core::LazyFormat& what) {
  OptixTraversableHandle handle = 0;
  return Check(optixAccelBuild(context, stream, &request.options,
                               request.inputs.data(),
                               static_cast<unsigned>(request.inputs.size()),
                               request.temp.address, request.temp.size,
                               request.output.address, request.output.size,
                               &handle, nullptr, 0),
               what)
      .transform([&] { return handle; });
}

Result<OptixTraversableHandle> TraversableHandle(OptixDeviceContext context,
                                                 CUdeviceptr pointer,
                                                 OptixTraversableType type,
                                                 const core::LazyFormat& what) {
  OptixTraversableHandle handle = 0;
  return Check(optixConvertPointerToTraversableHandle(context, pointer, type,
                                                      &handle),
               what)
      .transform([&] { return handle; });
}

}  // namespace umbra::ext::optix
