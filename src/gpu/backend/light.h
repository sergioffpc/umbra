// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Lights and cameras as a GPU holds them. The lights with a shape (sphere,
// rect, disk, cylinder) are custom primitives, one per instance, in the GAS
// of lights, which the IAS holds once.

#ifndef UMBRA_GPU_BACKEND_LIGHT_H_
#define UMBRA_GPU_BACKEND_LIGHT_H_

#include <driver_types.h>

#include <array>
#include <cstdint>

#include "accel.h"
#include "buffer.h"
#include "held.h"
#include "support.h"
#include "umbra/core/safety.h"
#include "umbra/device/render_data.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::gpu::backend {

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

// A light in a GPU's memory: its parameters, its placements and, for a
// light with a shape, the bounds of each instance, in world space, over the
// shutter.
struct GpuLight {
  device::LightId id{};
  DeviceBuffer record;
  HeldPlacements placements;
  DeviceBuffer bounds;
  std::uint32_t primitive_count = 0;
};

// Uploads `light`.
[[nodiscard]] Result<GpuLight> UploadLight(const device::Light& light,
                                           cudaStream_t stream);

// A light's instance among the primitives of the GAS of lights.
struct LightPrimitive {
  std::uint32_t light = 0;
  std::uint32_t instance = 0;
};

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

// A camera in a GPU's memory: its parameters and its camera-to-world keys.
struct GpuCamera {
  DeviceBuffer record;
  HeldPlacements placements;
};

// Uploads `camera`.
[[nodiscard]] Result<GpuCamera> UploadCamera(const device::Camera& camera,
                                             cudaStream_t stream);

}  // namespace umbra::gpu::backend

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_GPU_BACKEND_LIGHT_H_
