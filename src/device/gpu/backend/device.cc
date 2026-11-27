// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "device.h"

#include <driver_types.h>
#include <optix_types.h>

#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "accel.h"
#include "held.h"
#include "light.h"
#include "prototype.h"
#include "render.h"
#include "support.h"
#include "umbra/core/log.h"
#include "umbra/device/camera_rays.h"
#include "umbra/device/change_set.h"
#include "umbra/device/device.h"
#include "umbra/device/image.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/render_data.h"
#include "umbra/ext/cuda/owners.h"
#include "umbra/ext/cuda/runtime.h"
#include "umbra/ext/optix/optix.h"
#include "umbra/ext/optix/owners.h"

namespace umbra::device::gpu {
namespace {

using backend::kMinComputeCapabilityMajor;
using backend::Log;
using backend::Result;
using ext::cuda::DeviceBuffer;

// Fails when the GPU of CUDA ordinal `ordinal` is older than sm_120.
Result<void> CheckArchitecture(int ordinal) {
  const auto major = ext::cuda::ComputeCapabilityMajor(
      ordinal, std::format("cannot describe device {}", ordinal));
  if (!major.has_value()) {
    return std::unexpected(major.error());
  }
  if (*major < kMinComputeCapabilityMajor) {
    return std::unexpected(std::format("device {} is older than sm_{}0",
                                       ordinal, kMinComputeCapabilityMajor));
  }
  return {};
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

// What the IAS holds: the records of the prototypes' instances, then those
// of the lights' instances.
struct IasCounts {
  std::uint32_t instances = 0;
  std::uint32_t moving = 0;
  std::uint32_t lights = 0;
  std::uint32_t moving_lights = 0;
};

// The options of a build of a static acceleration structure.
OptixAccelBuildOptions StaticBuild() {
  return backend::BuildOptions(OPTIX_BUILD_FLAG_PREFER_FAST_TRACE,
                               OPTIX_BUILD_OPERATION_BUILD, 1);
}

using Lights = backend::IdTable<backend::GpuLight, device::LightId>;
using Prototypes = backend::IdTable<backend::GpuPrototype, device::PrototypeId>;

// The records the prims of `table` add to the IAS, and how many of them
// move.
template <typename Table>
std::pair<std::uint32_t, std::uint32_t> CountRecords(const Table& table) {
  std::pair<std::uint32_t, std::uint32_t> counts;
  table.ForEach([&](const auto& held) {
    const std::uint32_t records = backend::InstanceCount(held);
    counts.first += records;
    counts.second += records == 0 ? 0 : held.instances.moving_count;
  });
  return counts;
}

// What the IAS of `prototypes` and `lights` holds.
IasCounts CountIas(const Prototypes& prototypes, const Lights& lights) {
  const auto [instances, moving] = CountRecords(prototypes);
  const auto [light_instances, moving_lights] = CountRecords(lights);
  return {
      .instances = instances,
      .moving = moving,
      .lights = light_instances,
      .moving_lights = moving_lights,
  };
}

// Copies the records of every prim of `table` into `input`, from the record
// `first` on, and tells the record after the last it copied.
template <typename Table>
Result<std::uint32_t> CopyEachRecords(const Table& table, DeviceBuffer& input,
                                      std::uint32_t first,
                                      cudaStream_t stream) {
  Result<void> copied;
  table.ForEach([&](const auto& held) {
    copied = copied.and_then(
        [&] { return backend::CopyInstances(held, input, first, stream); });
    first += backend::InstanceCount(held);
  });
  return copied.transform([&] { return first; });
}

// The `total` records of the IAS: those of every prototype's instances, then
// those of every light's.
Result<DeviceBuffer> GatherInstances(const Prototypes& prototypes,
                                     const Lights& lights, std::uint32_t total,
                                     cudaStream_t stream) {
  auto input = DeviceBuffer::Allocate(total * sizeof(OptixInstance));
  if (!input.has_value()) {
    return input;
  }
  return CopyEachRecords(prototypes, *input, 0, stream)
      .and_then([&](std::uint32_t first) {
        return CopyEachRecords(lights, *input, first, stream);
      })
      .and_then([&](std::uint32_t /*end*/) { return std::move(input); });
}

}  // namespace

Result<std::unique_ptr<ModuleDevice>> ModuleDevice::Create(int ordinal) {
  if (auto usable = ext::optix::Initialize()
                        .and_then([&] { return CheckArchitecture(ordinal); })
                        .and_then([&] {
                          return ext::cuda::SetDevice(
                              ordinal,
                              std::format("cannot use device {}", ordinal));
                        });
      !usable.has_value()) {
    return std::unexpected(usable.error());
  }
  auto stream = ext::cuda::Stream::Create("cannot create a stream");
  if (!stream.has_value()) {
    return std::unexpected(stream.error());
  }
  auto context = ext::optix::DeviceContext::Create("cannot create a context");
  if (!context.has_value()) {
    return std::unexpected(context.error());
  }
  return std::make_unique<ModuleDevice>(ordinal, *std::move(stream),
                                        *std::move(context));
}

ModuleDevice::ModuleDevice(int ordinal, ext::cuda::Stream stream,
                           ext::optix::DeviceContext context)
    : ordinal_(ordinal),
      name_(std::format("gpu{}", ordinal)),
      stream_(std::move(stream)),
      context_(std::move(context)) {
  Log(core::LogLevel::kDebug, "{}: created, with its stream and context",
      name_);
}

ModuleDevice::~ModuleDevice() {
  if (auto current =
          ext::cuda::SetDevice(ordinal_, "cannot use it").and_then([&] {
            return ext::cuda::Synchronize(stream_.get(),
                                          "cannot wait for its work");
          });
      !current.has_value()) {
    Log(core::LogLevel::kWarning, "{}: {}", name_, current.error());
  }
  prototypes_.Clear();
  lights_.Clear();
  cameras_.Clear();
  ias_ = {};
  light_shapes_ = {};
  renderer_ = backend::Renderer();
  if (auto destroyed =
          context_.Destroy("cannot destroy its context").and_then([&] {
            return stream_.Destroy("cannot destroy its stream");
          });
      !destroyed.has_value()) {
    Log(core::LogLevel::kWarning, "{}: {}", name_, destroyed.error());
  }
}

backend::Gpu ModuleDevice::gpu() const {
  return {.context = context_.get(), .stream = stream_.get(), .name = name_};
}

Result<void> ModuleDevice::MakeCurrent() const {
  return ext::cuda::SetDevice(ordinal_,
                              std::format("cannot use device {}", ordinal_));
}

Result<void> ModuleDevice::Commit(const device::ChangeSet& changes) {
  if (auto current = MakeCurrent(); !current.has_value()) {
    return current;
  }
  prototypes_.Reserve(changes.ids.prototypes);
  lights_.Reserve(changes.ids.lights);
  cameras_.Reserve(changes.ids.cameras);
  const auto upload_light = [&](const device::Light& light) {
    return backend::UploadLight(light, gpu());
  };
  const auto upload_camera = [&](const device::Camera& camera) {
    return backend::UploadCamera(camera, stream_.get());
  };
  return ApplyPrototypes(changes.prototypes)
      .and_then(
          [&] { return ApplyWhole(changes.lights, lights_, upload_light); })
      .and_then(
          [&] { return ApplyWhole(changes.cameras, cameras_, upload_camera); })
      .and_then([&] { return BuildIas(); })
      .and_then([&] { return UploadLightShapes(changes.ids.lights); })
      .and_then([&] {
        return ext::cuda::Synchronize(stream_.get(), "the commit failed");
      });
}

Result<void> ModuleDevice::UploadLightShapes(std::uint32_t lights) {
  std::vector<device::LightShape> shapes(lights);
  lights_.ForEach([&](const backend::GpuLight& held) {
    const auto light = std::to_underlying(held.id);
    if (held.shape.has_value() && light < shapes.size()) {
      shapes.at(light) = *held.shape;
    }
  });
  return ext::cuda::Replace(light_shapes_,
                            std::span<const device::LightShape>(shapes),
                            stream_.get())
      .and_then([&] {
        return ext::cuda::Synchronize(stream_.get(),
                                      "cannot upload the lights' shapes");
      });
}

Result<void> ModuleDevice::PrepareRender(const device::FrameParameters& frame) {
  return MakeCurrent()
      .and_then([&] { return cameras_.Held(frame.camera); })
      .and_then([&](const backend::GpuCamera* camera) {
        const backend::Scene scene{
            .camera = device::MakeCameraModel(backend::ViewOf(*camera), frame),
            .top = ias_.handle,
            .light_shapes = light_shapes_.address(),
        };
        return renderer_.Prepare(frame, scene, context_.get(), stream_.get());
      });
}

Result<void> ModuleDevice::RenderTiles(
    const device::FrameParameters& frame,
    std::span<const device::PixelWindow> tiles) {
  return MakeCurrent()
      .and_then([&] { return renderer_.RenderTiles(tiles, stream_.get()); })
      .and_then(
          [&] { return renderer_.CopyTiles(frame, tiles, stream_.get()); });
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

Result<void> ModuleDevice::BuildIas() {
  const IasCounts counts = CountIas(prototypes_, lights_);
  const std::uint32_t total = counts.instances + counts.lights;
  if (total == 0) {
    ias_ = {};
    Log(core::LogLevel::kDebug, "{}: the IAS is empty", name_);
    return {};
  }
  auto input = GatherInstances(prototypes_, lights_, total, stream_.get());
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
        "{}: rebuilt the IAS: {} prototype instances ({} moving), {} light "
        "instances ({} moving)",
        name_, counts.instances, counts.moving, counts.lights,
        counts.moving_lights);
  });
}

}  // namespace umbra::device::gpu
