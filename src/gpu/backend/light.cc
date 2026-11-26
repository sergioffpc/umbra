// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "light.h"

#include <driver_types.h>
#include <optix_types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "buffer.h"
#include "held.h"
#include "support.h"
#include "umbra/core/safety.h"
#include "umbra/device/render_data.h"

namespace umbra::gpu::backend {
namespace {

// The bounds of a light's shape in the light's space.
struct LocalBounds {
  std::array<double, 3> low{};
  std::array<double, 3> high{};
};

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

// Fills the record of a light with what its shape gives, and tells the
// bounds of the shape: none for a light without one.
class ShapeVisitor {
 public:
  explicit ShapeVisitor(LightRecord& record UMBRA_LIFETIMEBOUND)
      : record_(&record) {}

  std::optional<LocalBounds> operator()(
      const device::Light::Sphere& sphere) const {
    Size(sphere.radius, 0.0);
    return Box(sphere.radius, sphere.radius, sphere.radius);
  }
  std::optional<LocalBounds> operator()(const device::Light::Rect& rect) const {
    Size(rect.width, rect.height);
    record_->texture = TextureOf(rect.texture);
    return Box(rect.width / 2.0, rect.height / 2.0, 0.0);
  }
  std::optional<LocalBounds> operator()(const device::Light::Disk& disk) const {
    Size(disk.radius, 0.0);
    return Box(disk.radius, disk.radius, 0.0);
  }
  std::optional<LocalBounds> operator()(
      const device::Light::Cylinder& cylinder) const {
    Size(cylinder.length, cylinder.radius);
    return Box(cylinder.length / 2.0, cylinder.radius, cylinder.radius);
  }
  std::optional<LocalBounds> operator()(
      const device::Light::Distant& distant) const {
    Size(distant.angle, 0.0);
    return std::nullopt;
  }
  std::optional<LocalBounds> operator()(const device::Light::Dome& dome) const {
    record_->texture = TextureOf(dome.texture);
    record_->dome_format = dome.format;
    record_->orientation = RowsOf(dome.orientation);
    return std::nullopt;
  }

 private:
  void Size(double first, double second) const {
    record_->size = FloatsOf(std::array{first, second});
  }
  static LocalBounds Box(double half_x, double half_y, double half_z) {
    return {
        .low = {-half_x, -half_y, -half_z},
        .high = {half_x, half_y, half_z},
    };
  }

  LightRecord* record_;
};

// Widens `box` to hold `point`.
void Extend(OptixAabb& box, const std::array<double, 3>& point) {
  box.minX = std::min(box.minX, static_cast<float>(point.at(0)));
  box.minY = std::min(box.minY, static_cast<float>(point.at(1)));
  box.minZ = std::min(box.minZ, static_cast<float>(point.at(2)));
  box.maxX = std::max(box.maxX, static_cast<float>(point.at(0)));
  box.maxY = std::max(box.maxY, static_cast<float>(point.at(1)));
  box.maxZ = std::max(box.maxZ, static_cast<float>(point.at(2)));
}

// `point` placed by `matrix`, for row vectors.
std::array<double, 3> Place(const std::array<double, 3>& point,
                            const device::Matrix& matrix) {
  std::array<double, 3> placed{};
  for (std::size_t axis = 0; axis < 3; ++axis) {
    placed.at(axis) =
        (point.at(0) * matrix.at(axis)) + (point.at(1) * matrix.at(4 + axis)) +
        (point.at(2) * matrix.at(8 + axis)) + matrix.at(12 + axis);
  }
  return placed;
}

// The transform of an instance that gives none.
constexpr std::array<device::Matrix, 1> kIdentity = {
    device::Matrix{
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
    },
};

// The world-space bounds of `local` placed by `instance`, over every key;
// an instance without one is untransformed.
OptixAabb BoundsOf(const LocalBounds& local, const device::Instance& instance) {
  constexpr float kInfinity = std::numeric_limits<float>::infinity();
  OptixAabb box{
      .minX = kInfinity,
      .minY = kInfinity,
      .minZ = kInfinity,
      .maxX = -kInfinity,
      .maxY = -kInfinity,
      .maxZ = -kInfinity,
  };
  const std::span<const device::Matrix> keys = instance.object_to_world.empty()
                                                   ? std::span(kIdentity)
                                                   : instance.object_to_world;
  for (const device::Matrix& matrix : keys) {
    for (unsigned corner = 0; corner < 8; ++corner) {
      const std::array<double, 3> point = {
          (corner & 1U) == 0U ? local.low.at(0) : local.high.at(0),
          (corner & 2U) == 0U ? local.low.at(1) : local.high.at(1),
          (corner & 4U) == 0U ? local.low.at(2) : local.high.at(2),
      };
      Extend(box, Place(point, matrix));
    }
  }
  return box;
}

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

// Uploads the bounds of each instance of a light whose shape has `local`
// bounds.
Result<void> UploadBounds(const std::optional<LocalBounds>& local,
                          const device::Light& light, GpuLight& held,
                          cudaStream_t stream) {
  if (!local.has_value()) {
    return {};
  }
  std::vector<OptixAabb> bounds;
  bounds.reserve(light.instances.size());
  for (const device::Instance& instance : light.instances) {
    bounds.push_back(BoundsOf(*local, instance));
  }
  held.primitive_count = static_cast<std::uint32_t>(bounds.size());
  return Replace(held.bounds, std::span<const OptixAabb>(bounds), stream);
}

}  // namespace

Result<GpuLight> UploadLight(const device::Light& light, cudaStream_t stream) {
  LightRecord record = RecordOf(light);
  const std::optional<LocalBounds> local =
      std::visit(ShapeVisitor(record), light.shape);
  const std::array records = {record};
  GpuLight held;
  held.id = light.id;
  return Replace(held.record, std::span<const LightRecord>(records), stream)
      .and_then([&] { return UploadPlacements(light.instances, stream); })
      .and_then([&](HeldPlacements placements) {
        held.placements = std::move(placements);
        return UploadBounds(local, light, held, stream);
      })
      .transform([&] { return std::move(held); });
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
  return Replace(held.record, std::span<const CameraRecord>(records), stream)
      .and_then([&] { return UploadPlacements(placement, stream); })
      .transform([&](HeldPlacements placements) {
        held.placements = std::move(placements);
        return std::move(held);
      });
}

}  // namespace umbra::gpu::backend
