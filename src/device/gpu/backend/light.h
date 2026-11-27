// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Lights and cameras as a GPU holds them. A light with a shape (sphere,
// rect, disk, cylinder) has a GAS of its own, of one custom primitive that
// bounds the shape in the light's space, which the IAS holds once per
// instance of the light, as it holds a prototype's.

#ifndef UMBRA_DEVICE_GPU_BACKEND_LIGHT_H_
#define UMBRA_DEVICE_GPU_BACKEND_LIGHT_H_

#include <driver_types.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "accel.h"
#include "held.h"
#include "instances.h"
#include "support.h"
#include "umbra/core/safety.h"
#include "umbra/device/light_shapes.h"
#include "umbra/device/render_data.h"
#include "umbra/ext/cuda/owners.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device::gpu::backend {

// The texture id of a light that has no texture.
inline constexpr std::uint32_t kNoTexture = ~std::uint32_t{0};

// A light's parameters, in a GPU's memory.
struct LightRecord {
  // The index of its shape in device::Light::Shape.
  std::uint32_t shape = 0;
  std::uint32_t texture = kNoTexture;
  device::Normalization normalization = device::Normalization::kUnnormalized;
  device::DomeTextureFormat dome_format = device::DomeTextureFormat::kAutomatic;
  // The radius of a sphere or a disk, the width and height of a rect, the
  // length and radius of a cylinder, the angle of a distant light.
  std::array<float, 2> size{};
  float intensity = 1.0F;
  float exposure = 0.0F;
  std::array<float, 3> color{};
  // 0 for none.
  float color_temperature = 0.0F;
  float cone_angle = 180.0F;
  std::uint32_t instance_count = 0;
  // A dome's orientation, as RowsOf gives it.
  std::array<float, 12> orientation{};
};

// A light in a GPU's memory: its parameters, its placements, the tilt
// factor of each, and, for a light with a shape, its GAS and the records of
// its instances in the IAS.
struct GpuLight {
  device::LightId id{};
  // For the messages.
  std::string path;
  ext::cuda::DeviceBuffer record;
  HeldPlacements placements;
  // A float per instance, in the order of its placements, which scales its
  // emission.
  ext::cuda::DeviceBuffer tilt_factors;
  // Its shape, when rays meet it, which its GAS bounds.
  std::optional<device::LightShape> shape;
  AccelStructure gas;
  InstanceRecords instances;
};

// Uploads `light` and, when it has a shape, builds its GAS and the records
// of its instances.
[[nodiscard]] Result<GpuLight> UploadLight(const device::Light& light,
                                           const Gpu& gpu);

// The records `held` adds to the IAS: none when it has no shape.
[[nodiscard]] std::uint32_t InstanceCount(const GpuLight& held);

// Copies the records of `held` into `input`, an array of OptixInstance, from
// the record `first` on.
[[nodiscard]] Result<void> CopyInstances(const GpuLight& held,
                                         ext::cuda::DeviceBuffer& input,
                                         std::uint32_t first,
                                         cudaStream_t stream);

// A camera's parameters, in a GPU's memory.
struct CameraRecord {
  device::Projection projection = device::Projection::kPerspective;
  float focal_length = 0.0F;
  std::array<float, 2> aperture{};
  std::array<float, 2> aperture_offset{};
  std::array<float, 2> clipping_range{};
  float f_stop = 0.0F;
  float focus_distance = 0.0F;
};

// A camera in a GPU's memory: its parameters and its camera-to-world keys,
// and the host's copy of them, which the camera model of each render
// product is made from.
struct GpuCamera {
  ext::cuda::DeviceBuffer record;
  HeldPlacements placements;
  // The camera, without its path, its camera_to_world empty.
  device::Camera parameters;
  std::vector<device::Matrix> camera_to_world;
};

// The camera `held` holds, as the host made it.
[[nodiscard]] device::Camera ViewOf(const GpuCamera& held UMBRA_LIFETIMEBOUND);

// Uploads `camera`.
[[nodiscard]] Result<GpuCamera> UploadCamera(const device::Camera& camera,
                                             cudaStream_t stream);

}  // namespace umbra::device::gpu::backend

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_GPU_BACKEND_LIGHT_H_
