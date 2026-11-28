// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "light.h"

#include <driver_types.h>
#include <optix_types.h>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <variant>

#include "accel.h"
#include "held.h"
#include "instances.h"
#include "support.h"
#include "umbra/core/log.h"
#include "umbra/core/safety.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/render_data.h"
#include "umbra/ext/cuda/owners.h"
#include "umbra/ext/cuda/runtime.h"

namespace umbra::device::gpu::backend {

using ext::cuda::DeviceBuffer;
using ext::cuda::Replace;

namespace {

// The id of `texture`, or kNoTexture.
std::uint32_t TextureOf(const std::optional<device::TextureId>& texture) {
  return texture.has_value() ? std::to_underlying(*texture) : kNoTexture;
}

// `values`, as floats.
std::array<float, 2> FloatsOf(const std::array<double, 2>& values) {
  return {
      static_cast<float>(values.at(0)),
      static_cast<float>(values.at(1)),
  };
}

// Fills the record of a light with what its shape gives.
class ShapeVisitor {
 public:
  explicit ShapeVisitor(LightRecord& record UMBRA_LIFETIMEBOUND)
      : record_(&record) {}

  void operator()(const device::Light::Sphere& sphere) const {
    Size(sphere.radius, 0.0);
  }
  void operator()(const device::Light::Rect& rect) const {
    Size(rect.width, rect.height);
    record_->texture = TextureOf(rect.texture);
  }
  void operator()(const device::Light::Disk& disk) const {
    Size(disk.radius, 0.0);
  }
  void operator()(const device::Light::Cylinder& cylinder) const {
    Size(cylinder.length, cylinder.radius);
  }
  void operator()(const device::Light::Distant& distant) const {
    Size(distant.angle, 0.0);
  }
  void operator()(const device::Light::Dome& dome) const {
    record_->texture = TextureOf(dome.texture);
    record_->dome_format = dome.format;
    record_->orientation = RowsOf(dome.orientation);
  }
  void operator()(const device::Light::Portal& portal) const {
    Size(portal.width, portal.height);
  }

 private:
  void Size(double first, double second) const {
    record_->size = FloatsOf(std::array{first, second});
  }

  LightRecord* record_;
};

// The parameters of `light` but those of its shape.
LightRecord RecordOf(const device::Light& light) {
  LightRecord record{};
  record.shape = static_cast<std::uint32_t>(light.shape.index());
  record.normalization = light.normalization;
  record.intensity = light.intensity;
  record.exposure = light.exposure;
  record.color = light.color.rgb;
  record.color_temperature = light.color_temperature.value_or(0.0F);
  record.cone_angle = light.cone_angle;
  record.instance_count = static_cast<std::uint32_t>(light.instances.size());
  return record;
}

// The GAS of a light whose shape has `bounds`: one custom primitive.
Result<void> BuildLightGas(const device::ShapeBounds& bounds, GpuLight& held,
                           const Gpu& gpu) {
  const std::array boxes = {
      OptixAabb{
          .minX = bounds.lower.x,
          .minY = bounds.lower.y,
          .minZ = bounds.lower.z,
          .maxX = bounds.upper.x,
          .maxY = bounds.upper.y,
          .maxZ = bounds.upper.z,
      },
  };
  auto uploaded =
      DeviceBuffer::UploadOf(std::span<const OptixAabb>(boxes), gpu.stream);
  if (!uploaded.has_value()) {
    return std::unexpected(uploaded.error());
  }
  const CUdeviceptr address = uploaded->address();
  const unsigned flags = OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT;
  OptixBuildInputCustomPrimitiveArray custom{};
  custom.aabbBuffers = &address;
  custom.numPrimitives = 1;
  custom.flags = &flags;
  custom.numSbtRecords = 1;
  const std::array inputs = {
      OptixBuildInput{
          .type = OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES,
          .customPrimitiveArray = custom,
      },
  };
  return BuildAccel(gpu,
                    BuildOptions(OPTIX_BUILD_FLAG_PREFER_FAST_TRACE,
                                 OPTIX_BUILD_OPERATION_BUILD, 1),
                    inputs, held.gas);
}

// Builds the GAS of `held`, a light with a shape that rays meet, if it has
// one, from the shape's bounds, and the records of its instances. A distant
// light, a dome and a portal have none: a portal emits nothing and only
// guides its domes' sampling, so rays never hit it.
Result<void> BuildShape(const device::Light& light, GpuLight& held,
                        const Gpu& gpu) {
  if (!held.shape.has_value()) {
    return {};
  }
  const OrderedInstances ordered = Order(light.instances);
  const RecordKind kind{
      .prim = std::to_underlying(light.id),
      .sbt_offset = kLightHitGroup,
      .mask = kLightMask,
  };
  return BuildLightGas(device::BoundsOf(*held.shape), held, gpu)
      .and_then([&] {
        return UploadRecords(ordered, kind, held.gas.handle, held.instances,
                             gpu);
      })
      .transform([&] {
        Log(core::LogLevel::kDebug,
            "{}: built the GAS of light {}: a {}, {} instances ({} moving)",
            gpu.name, held.path,
            device::kLightShapeNames.at(light.shape.index()),
            CountOf(held.instances), held.instances.moving_count);
      });
}

}  // namespace

Result<GpuLight> UploadLight(const device::Light& light, const Gpu& gpu) {
  LightRecord record = RecordOf(light);
  std::visit(ShapeVisitor(record), light.shape);
  const std::array records = {record};
  GpuLight held;
  held.id = light.id;
  held.path = light.path;
  held.shape = device::ShapeOf(light.shape);
  return Replace(held.record, std::span<const LightRecord>(records), gpu.stream)
      .and_then([&] { return UploadPlacements(light.instances, gpu.stream); })
      .and_then([&](HeldPlacements placements) {
        held.placements = std::move(placements);
        return Replace(held.tilt_factors, light.tilt_factors, gpu.stream);
      })
      .and_then([&] { return BuildShape(light, held, gpu); })
      .and_then([&] {
        return ext::cuda::Synchronize(gpu.stream, "cannot upload a light");
      })
      .transform([&] { return std::move(held); });
}

std::uint32_t InstanceCount(const GpuLight& held) {
  return held.gas.handle == 0 ? 0 : CountOf(held.instances);
}

Result<void> CopyInstances(const GpuLight& held, DeviceBuffer& input,
                           std::uint32_t first, cudaStream_t stream) {
  if (InstanceCount(held) == 0) {
    return {};
  }
  return CopyRecords(held.instances, input, first, stream);
}

Result<GpuCamera> UploadCamera(const device::Camera& camera,
                               cudaStream_t stream) {
  const std::array records = {
      CameraRecord{
          .projection = camera.projection,
          .focal_length = static_cast<float>(camera.focal_length),
          .aperture = FloatsOf(camera.aperture),
          .aperture_offset = FloatsOf(camera.aperture_offset),
          .clipping_range = FloatsOf(camera.clipping_range),
          .f_stop = static_cast<float>(camera.f_stop),
          .focus_distance = static_cast<float>(camera.focus_distance),
      },
  };
  const std::array placement = {
      device::Instance{
          .object_to_world = camera.camera_to_world,
          .primvars = {},
      },
  };
  GpuCamera held;
  held.parameters = camera;
  held.parameters.path = {};
  held.parameters.camera_to_world = {};
  held.camera_to_world.assign(camera.camera_to_world.begin(),
                              camera.camera_to_world.end());
  return Replace(held.record, std::span<const CameraRecord>(records), stream)
      .and_then([&] { return UploadPlacements(placement, stream); })
      .transform([&](HeldPlacements placements) {
        held.placements = std::move(placements);
        return std::move(held);
      });
}

device::Camera ViewOf(const GpuCamera& held) {
  device::Camera camera = held.parameters;
  camera.camera_to_world = held.camera_to_world;
  return camera;
}

}  // namespace umbra::device::gpu::backend
