// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "pipeline.h"

#include <driver_types.h>
#include <optix_types.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <ranges>
#include <span>
#include <utility>

#include "launch.h"
#include "support.h"
#include "umbra/core/safety.h"
#include "umbra/ext/cuda/owners.h"
#include "umbra/ext/optix/host.h"
#include "umbra/ext/optix/owners.h"

namespace umbra::device::gpu::backend {

using ext::cuda::DeviceBuffer;

// The PTX of placeholder.cu, embedded by umbra_add_optix_programs.
std::span<const char> PlaceholderPrograms();

namespace {

// Whether the programs were compiled for source-level debugging.
constexpr bool kDeviceDebug = UMBRA_GPU_DEVICE_DEBUG == 1;

// The values of a ray's payload: the distance and the normal of its hit.
constexpr int kPayloadValues = 4;
// The attributes of a light shape's hit: its normal.
constexpr int kAttributeValues = 3;
// The IAS, a motion transform and a GAS.
constexpr unsigned kTraversableGraphDepth = 3;

OptixPipelineCompileOptions PipelineOptions() {
  OptixPipelineCompileOptions options{};
  options.usesMotionBlur = 1;
  options.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_ANY;
  options.numPayloadValues = kPayloadValues;
  options.numAttributeValues = kAttributeValues;
  options.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
  options.pipelineLaunchParamsVariableName = "params";
  options.pipelineLaunchParamsSizeInBytes = sizeof(LaunchParams);
  options.usesPrimitiveTypeFlags =
      static_cast<unsigned>(OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE) |
      static_cast<unsigned>(OPTIX_PRIMITIVE_TYPE_FLAGS_CUSTOM);
  return options;
}

Result<ext::optix::Module> CreateModule(OptixDeviceContext context) {
  OptixModuleCompileOptions options{};
  options.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
  options.optLevel = kDeviceDebug ? OPTIX_COMPILE_OPTIMIZATION_LEVEL_0
                                  : OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
  options.debugLevel = kDeviceDebug ? OPTIX_COMPILE_DEBUG_LEVEL_FULL
                                    : OPTIX_COMPILE_DEBUG_LEVEL_MINIMAL;
  return ext::optix::CreateModule(context, options, PipelineOptions(),
                                  PlaceholderPrograms(),
                                  "cannot compile the programs");
}

// The description of a hit group of `module`'s closest-hit program
// `closest_hit` and intersection program `intersection`, if any.
OptixProgramGroupDesc HitGroup(OptixModule module, const char* closest_hit,
                               const char* UMBRA_NULLABLE intersection) {
  return {
      .kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP,
      .flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE,
      .hitgroup =
          {
              .moduleCH = module,
              .entryFunctionNameCH = closest_hit,
              .moduleAH = nullptr,
              .entryFunctionNameAH = nullptr,
              .moduleIS = intersection == nullptr ? nullptr : module,
              .entryFunctionNameIS = intersection,
          },
  };
}

// The descriptions of the program groups, in the order of kProgramGroups.
std::array<OptixProgramGroupDesc, kProgramGroups> GroupDescriptions(
    OptixModule module) {
  return {
      OptixProgramGroupDesc{
          .kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN,
          .flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE,
          .raygen =
              {
                  .module = module,
                  .entryFunctionName = "__raygen__placeholder",
              },
      },
      OptixProgramGroupDesc{
          .kind = OPTIX_PROGRAM_GROUP_KIND_MISS,
          .flags = OPTIX_PROGRAM_GROUP_FLAGS_NONE,
          .miss =
              {
                  .module = module,
                  .entryFunctionName = "__miss__placeholder",
              },
      },
      HitGroup(module, "__closesthit__triangle", nullptr),
      HitGroup(module, "__closesthit__light", "__intersection__light_shape"),
  };
}

Result<void> CreateGroups(
    OptixDeviceContext context, OptixModule module,
    std::array<ext::optix::ProgramGroup, kProgramGroups>& owned) {
  const auto descriptions = GroupDescriptions(module);
  return ext::optix::CreateProgramGroups(context, descriptions,
                                         OptixProgramGroupOptions{}, owned,
                                         "cannot create the program groups");
}

Result<ext::optix::Pipeline> CreatePipeline(
    OptixDeviceContext context,
    const std::array<ext::optix::ProgramGroup, kProgramGroups>& owned) {
  std::array<OptixProgramGroup, kProgramGroups> groups{};
  std::ranges::transform(
      owned, groups.begin(),
      [](const ext::optix::ProgramGroup& group) { return group.get(); });
  OptixPipelineLinkOptions link{};
  link.maxTraceDepth = 1;
  return ext::optix::CreatePipeline(context, PipelineOptions(), link, groups,
                                    "cannot link the pipeline")
      .and_then(
          [&](ext::optix::Pipeline pipeline) -> Result<ext::optix::Pipeline> {
            return ext::optix::SetStackSizes(
                       pipeline.get(),
                       {
                           .trace = 1,
                           .continuation_callable = 0,
                           .direct_callable_from_state = 0,
                           .direct_callable_from_traversal = 0,
                           .traversable_graph = kTraversableGraphDepth,
                       },
                       "cannot size the pipeline's stacks")
                .transform([&] { return std::move(pipeline); });
          });
}

// An SBT record with no data: its header only.
struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) Record {
  std::array<char, OPTIX_SBT_RECORD_HEADER_SIZE> header{};
};

// The records of `groups`, one each, in their order.
Result<std::array<Record, kProgramGroups>> PackRecords(
    const std::array<ext::optix::ProgramGroup, kProgramGroups>& groups) {
  std::array<Record, kProgramGroups> records{};
  for (auto [group, record] : std::views::zip(groups, records)) {
    if (auto packed = ext::optix::PackHeader(group.get(), record.header,
                                             "cannot pack an SBT record");
        !packed.has_value()) {
      return std::unexpected(packed.error());
    }
  }
  return records;
}

// The SBT of the records at `address`, in the order of kProgramGroups.
OptixShaderBindingTable TableOf(CUdeviceptr address) {
  constexpr CUdeviceptr kStride = sizeof(Record);
  OptixShaderBindingTable sbt{};
  sbt.raygenRecord = address;
  sbt.missRecordBase = address + kStride;
  sbt.missRecordStrideInBytes = static_cast<unsigned>(kStride);
  sbt.missRecordCount = 1;
  sbt.hitgroupRecordBase = address + (2 * kStride);
  sbt.hitgroupRecordStrideInBytes = static_cast<unsigned>(kStride);
  sbt.hitgroupRecordCount = 2;
  return sbt;
}

}  // namespace

Result<Pipeline> Pipeline::Create(OptixDeviceContext context,
                                  cudaStream_t stream) {
  Pipeline made;
  auto module = CreateModule(context);
  if (!module.has_value()) {
    return std::unexpected(module.error());
  }
  made.module_ = *std::move(module);
  auto built =
      CreateGroups(context, made.module_.get(), made.groups_)
          .and_then([&] { return CreatePipeline(context, made.groups_); })
          .and_then([&](ext::optix::Pipeline pipeline) {
            made.pipeline_ = std::move(pipeline);
            return PackRecords(made.groups_);
          })
          .and_then([&](const std::array<Record, kProgramGroups>& records) {
            return DeviceBuffer::UploadOf(std::span<const Record>(records),
                                          stream);
          });
  if (!built.has_value()) {
    return std::unexpected(built.error());
  }
  made.records_ = *std::move(built);
  made.sbt_ = TableOf(made.records_.address());
  return made;
}

Result<void> Pipeline::Launch(CUdeviceptr params, std::uint32_t width,
                              std::uint32_t height, std::uint32_t depth,
                              cudaStream_t stream) const {
  return ext::optix::Launch(
      pipeline_.get(), stream,
      {
          .params = {.address = params, .size = sizeof(LaunchParams)},
          .sbt = sbt_,
          .size = {.width = width, .height = height, .depth = depth},
      },
      "cannot launch the programs");
}

}  // namespace umbra::device::gpu::backend
