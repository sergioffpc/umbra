// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "device.h"

#include <cuda_runtime_api.h>
#include <driver_types.h>
#include <optix_host.h>
#include <optix_stubs.h>
#include <optix_types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "accel.h"
#include "buffer.h"
#include "held.h"
#include "light.h"
#include "prototype.h"
#include "support.h"
#include "umbra/core/log.h"
#include "umbra/core/safety.h"
#include "umbra/device/change_set.h"
#include "umbra/device/render_data.h"

namespace umbra::gpu {
namespace {

using backend::CheckCuda;
using backend::CheckOptix;
using backend::DeviceBuffer;
using backend::kMinComputeCapabilityMajor;
using backend::Log;
using backend::Result;

// OptiX's warnings and errors, as the gpu component's.
void LogFromOptix(unsigned level, const char* UMBRA_NULLABLE tag,
                  const char* UMBRA_NULLABLE message,
                  void* UMBRA_NULLABLE /*data*/) {
  constexpr unsigned kWarningLevel = 3;
  Log(level <= kWarningLevel ? core::LogLevel::kWarning
                             : core::LogLevel::kDebug,
      "OptiX: {}: {}", tag == nullptr ? "" : tag,
      message == nullptr ? "" : message);
}

// Fails when the GPU of CUDA ordinal `ordinal` is older than sm_120.
Result<void> CheckArchitecture(int ordinal) {
  int major = 0;
  if (auto read =
          CheckCuda(cudaDeviceGetAttribute(
                        &major, cudaDevAttrComputeCapabilityMajor, ordinal),
                    std::format("cannot describe device {}", ordinal));
      !read.has_value()) {
    return read;
  }
  if (major < kMinComputeCapabilityMajor) {
    return std::unexpected(std::format("device {} is older than sm_{}0",
                                       ordinal, kMinComputeCapabilityMajor));
  }
  return {};
}

// An OptiX context on the current GPU.
Result<OptixDeviceContext> CreateContext() {
  OptixDeviceContextOptions options{};
  options.logCallbackFunction = LogFromOptix;
  options.logCallbackLevel = 3;
  OptixDeviceContext context = nullptr;
  // The current CUDA context, which the runtime made for the current GPU.
  return CheckOptix(optixDeviceContextCreate(nullptr, &options, &context),
                    "cannot create a context")
      .transform([&] { return context; });
}

// Calls `apply` on each of `items`, in order, until one fails.
template <typename Range, typename Apply>
Result<void> ApplyEach(const Range& items, const Apply& apply) {
  for (const auto& item : items) {
    if (auto done = apply(item); !done.has_value()) {
      return done;
    }
  }
  return {};
}

// Replaces `held` with what `upload` makes of `view`.
template <typename T, typename View, typename Upload>
Result<void> UploadInto(T& held, const View& view, const Upload& upload) {
  return upload(view).transform(
      [&](T uploaded) { held = std::move(uploaded); });
}

// Applies the removals, updates and additions of `changes` to `table`:
// `upload` uploads a light or a camera, which is updated whole.
template <typename T, typename View, typename Id, typename Upload>
Result<void> ApplyWhole(const device::Changes<View, Id>& changes,
                        backend::IdTable<T, Id>& table, const Upload& upload) {
  const auto update = [&](const View& updated) {
    return table.Held(updated.id).and_then([&](T* held) {
      return UploadInto(*held, updated, upload);
    });
  };
  const auto add = [&](const View& added) {
    return upload(added).and_then(
        [&](T uploaded) { return table.Add(added.id, std::move(uploaded)); });
  };
  return ApplyEach(changes.removed,
                   [&](Id removed) { return table.Remove(removed); })
      .and_then([&] { return ApplyEach(changes.updated, update); })
      .and_then([&] { return ApplyEach(changes.added, add); });
}

// Whether `changes` adds, updates or removes any prim.
template <typename Changes>
bool HasChanges(const Changes& changes) {
  return !changes.added.empty() || !changes.updated.empty() ||
         !changes.removed.empty();
}

// The record of the GAS of lights in the IAS, untransformed.
OptixInstance LightsRecord(OptixTraversableHandle handle) {
  OptixInstance record{};
  constexpr std::array<float, 12> kIdentity = {
      1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F,
  };
  std::ranges::copy(kIdentity, std::begin(record.transform));
  record.sbtOffset = backend::kLightHitGroup;
  record.visibilityMask = backend::kLightMask;
  record.flags = OPTIX_INSTANCE_FLAG_DISABLE_TRIANGLE_FACE_CULLING;
  record.traversableHandle = handle;
  return record;
}

// What the IAS holds: the records of the prototypes' instances, and that of
// the GAS of lights.
struct IasCounts {
  std::uint32_t instances = 0;
  std::uint32_t moving = 0;
  std::uint32_t lights = 0;
};

// The options of a build of a static acceleration structure.
OptixAccelBuildOptions StaticBuild() {
  return backend::BuildOptions(OPTIX_BUILD_FLAG_PREFER_FAST_TRACE,
                               OPTIX_BUILD_OPERATION_BUILD, 1);
}

using Lights = backend::IdTable<backend::GpuLight, device::LightId>;
using Prototypes = backend::IdTable<backend::GpuPrototype, device::PrototypeId>;

// The primitives of the GAS of lights: every instance of every light with a
// shape, light by light.
std::vector<backend::LightPrimitive> LightPrimitives(const Lights& lights) {
  std::vector<backend::LightPrimitive> primitives;
  lights.ForEach([&](const backend::GpuLight& light) {
    for (std::uint32_t instance = 0; instance < light.primitive_count;
         ++instance) {
      primitives.push_back(
          {.light = std::to_underlying(light.id), .instance = instance});
    }
  });
  return primitives;
}

// The bounds of the `count` primitives of the GAS of lights, in their order.
Result<DeviceBuffer> GatherBounds(const Lights& lights, std::size_t count,
                                  cudaStream_t stream) {
  auto bounds = DeviceBuffer::Allocate(count * sizeof(OptixAabb));
  if (!bounds.has_value()) {
    return bounds;
  }
  std::size_t offset = 0;
  Result<void> copied;
  lights.ForEach([&](const backend::GpuLight& light) {
    copied = copied.and_then(
        [&] { return bounds->CopyFrom(light.bounds, offset, stream); });
    offset += light.bounds.size();
  });
  return copied.and_then([&] { return std::move(bounds); });
}

// What the IAS of `prototypes` and `light_gas` holds.
IasCounts CountIas(const Prototypes& prototypes,
                   const backend::AccelStructure& light_gas) {
  IasCounts counts{.lights = light_gas.handle == 0 ? 0U : 1U};
  prototypes.ForEach([&](const backend::GpuPrototype& prototype) {
    const std::uint32_t records = backend::InstanceCount(prototype);
    counts.instances += records;
    counts.moving += records == 0 ? 0 : prototype.instances.moving_count;
  });
  return counts;
}

// Copies the records of every prototype's instances into `input`, from the
// first on, and tells how many it copied.
Result<std::uint32_t> CopyPrototypeInstances(const Prototypes& prototypes,
                                             DeviceBuffer& input,
                                             cudaStream_t stream) {
  std::uint32_t first = 0;
  Result<void> copied;
  prototypes.ForEach([&](const backend::GpuPrototype& prototype) {
    const std::uint32_t records = backend::InstanceCount(prototype);
    copied = copied.and_then([&] {
      return backend::CopyInstances(prototype, input, first, stream);
    });
    first += records;
  });
  return copied.transform([&] { return first; });
}

// The `total` records of the IAS: those of every prototype's instances, then
// that of the GAS of lights.
Result<DeviceBuffer> GatherInstances(const Prototypes& prototypes,
                                     const backend::AccelStructure& light_gas,
                                     std::uint32_t total, cudaStream_t stream) {
  auto input = DeviceBuffer::Allocate(total * sizeof(OptixInstance));
  if (!input.has_value()) {
    return input;
  }
  const std::array lights = {LightsRecord(light_gas.handle)};
  return CopyPrototypeInstances(prototypes, *input, stream)
      .and_then([&](std::uint32_t first) -> Result<void> {
        if (light_gas.handle == 0) {
          return {};
        }
        return input->Write(std::as_bytes(std::span(lights)),
                            first * sizeof(OptixInstance), stream);
      })
      .and_then([&] { return std::move(input); });
}

}  // namespace

Result<std::unique_ptr<ModuleDevice>> ModuleDevice::Create(int ordinal) {
  if (auto usable = backend::InitOptix()
                        .and_then([&] { return CheckArchitecture(ordinal); })
                        .and_then([&] {
                          return CheckCuda(
                              cudaSetDevice(ordinal),
                              std::format("cannot use device {}", ordinal));
                        });
      !usable.has_value()) {
    return std::unexpected(usable.error());
  }
  cudaStream_t stream = nullptr;
  if (auto created =
          CheckCuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking),
                    "cannot create a stream");
      !created.has_value()) {
    return std::unexpected(created.error());
  }
  auto context = CreateContext();
  if (!context.has_value()) {
    static_cast<void>(cudaStreamDestroy(stream));
    return std::unexpected(context.error());
  }
  return std::make_unique<ModuleDevice>(ordinal, stream, *context);
}

ModuleDevice::ModuleDevice(int ordinal, cudaStream_t stream,
                           OptixDeviceContext context)
    : ordinal_(ordinal),
      name_(std::format("gpu{}", ordinal)),
      stream_(stream),
      context_(context) {
  Log(core::LogLevel::kDebug, "{}: created, with its stream and context",
      name_);
}

ModuleDevice::~ModuleDevice() {
  if (auto current =
          CheckCuda(cudaSetDevice(ordinal_), "cannot use it").and_then([&] {
            return CheckCuda(cudaStreamSynchronize(stream_),
                             "cannot wait for its work");
          });
      !current.has_value()) {
    Log(core::LogLevel::kWarning, "{}: {}", name_, current.error());
  }
  prototypes_.Clear();
  lights_.Clear();
  cameras_.Clear();
  light_gas_ = {};
  light_primitives_ = {};
  ias_ = {};
  if (auto destroyed = CheckOptix(optixDeviceContextDestroy(context_),
                                  "cannot destroy its context")
                           .and_then([&] {
                             return CheckCuda(cudaStreamDestroy(stream_),
                                              "cannot destroy its stream");
                           });
      !destroyed.has_value()) {
    Log(core::LogLevel::kWarning, "{}: {}", name_, destroyed.error());
  }
}

backend::Gpu ModuleDevice::gpu() const {
  return {.context = context_, .stream = stream_, .name = name_};
}

Result<void> ModuleDevice::Commit(const device::ChangeSet& changes) {
  if (auto current = CheckCuda(cudaSetDevice(ordinal_),
                               std::format("cannot use device {}", ordinal_));
      !current.has_value()) {
    return current;
  }
  prototypes_.Reserve(changes.ids.prototypes);
  lights_.Reserve(changes.ids.lights);
  cameras_.Reserve(changes.ids.cameras);
  const auto upload_light = [&](const device::Light& light) {
    return backend::UploadLight(light, stream_);
  };
  const auto upload_camera = [&](const device::Camera& camera) {
    return backend::UploadCamera(camera, stream_);
  };
  const bool lights_change = HasChanges(changes.lights);
  return ApplyPrototypes(changes.prototypes)
      .and_then(
          [&] { return ApplyWhole(changes.lights, lights_, upload_light); })
      .and_then(
          [&] { return ApplyWhole(changes.cameras, cameras_, upload_camera); })
      .and_then(
          [&] { return lights_change ? BuildLightGas() : Result<void>(); })
      .and_then([&] { return BuildIas(); })
      .and_then([&] {
        return CheckCuda(cudaStreamSynchronize(stream_), "the commit failed");
      });
}

Result<void> ModuleDevice::ApplyPrototypes(
    const device::PrototypeChanges& changes) {
  const auto update = [&](const device::UpdatedPrototype& updated) {
    return prototypes_.Held(updated.prototype.id)
        .and_then([&](backend::GpuPrototype* held) {
          return backend::UpdatePrototype(updated, *held, gpu());
        });
  };
  const auto add = [&](const device::Prototype& added) {
    return backend::AddPrototype(added, gpu())
        .and_then([&](backend::GpuPrototype held) {
          return prototypes_.Add(added.id, std::move(held));
        });
  };
  return ApplyEach(changes.removed,
                   [&](device::PrototypeId removed) {
                     return prototypes_.Remove(removed);
                   })
      .and_then([&] { return ApplyEach(changes.updated, update); })
      .and_then([&] { return ApplyEach(changes.added, add); });
}

Result<void> ModuleDevice::BuildLightGas() {
  const std::vector<backend::LightPrimitive> primitives =
      LightPrimitives(lights_);
  if (primitives.empty()) {
    light_gas_ = {};
    light_primitives_ = {};
    Log(core::LogLevel::kDebug, "{}: no light has a shape", name_);
    return {};
  }
  auto bounds = GatherBounds(lights_, primitives.size(), stream_);
  if (!bounds.has_value()) {
    return std::unexpected(bounds.error());
  }
  const CUdeviceptr address = bounds->address();
  const unsigned flags = OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT;
  OptixBuildInputCustomPrimitiveArray custom{};
  custom.aabbBuffers = &address;
  custom.numPrimitives = static_cast<unsigned>(primitives.size());
  custom.flags = &flags;
  custom.numSbtRecords = 1;
  const std::array inputs = {
      OptixBuildInput{
          .type = OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES,
          .customPrimitiveArray = custom,
      },
  };
  return backend::Replace(light_primitives_,
                          std::span<const backend::LightPrimitive>(primitives),
                          stream_)
      .and_then([&] {
        return backend::BuildAccel(gpu(), StaticBuild(), inputs, light_gas_);
      })
      .transform([&] {
        Log(core::LogLevel::kDebug,
            "{}: built the GAS of lights: {} shaped light instances", name_,
            primitives.size());
      });
}

Result<void> ModuleDevice::BuildIas() {
  const IasCounts counts = CountIas(prototypes_, light_gas_);
  const std::uint32_t total = counts.instances + counts.lights;
  if (total == 0) {
    ias_ = {};
    Log(core::LogLevel::kDebug, "{}: the IAS is empty", name_);
    return {};
  }
  auto input = GatherInstances(prototypes_, light_gas_, total, stream_);
  if (!input.has_value()) {
    return std::unexpected(input.error());
  }
  OptixBuildInputInstanceArray instances{};
  instances.instances = input->address();
  instances.numInstances = total;
  const std::array inputs = {
      OptixBuildInput{
          .type = OPTIX_BUILD_INPUT_TYPE_INSTANCES,
          .instanceArray = instances,
      },
  };
  return backend::BuildAccel(gpu(), StaticBuild(), inputs, ias_).transform([&] {
    Log(core::LogLevel::kDebug,
        "{}: rebuilt the IAS: {} prototype instances ({} moving), {} GAS "
        "of lights",
        name_, counts.instances, counts.moving, counts.lights);
  });
}

}  // namespace umbra::gpu
