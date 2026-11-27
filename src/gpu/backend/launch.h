// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// What a launch of the placeholder integrator's programs reads, as the host
// writes it and the programs (placeholder.cu) read it: plain data, with the
// GPU's addresses as integers.

#ifndef UMBRA_GPU_BACKEND_LAUNCH_H_
#define UMBRA_GPU_BACKEND_LAUNCH_H_

#include <optix_types.h>

#include <cstdint>

#include "umbra/core/safety.h"
#include "umbra/device/camera_rays.h"
#include "umbra/device/image.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::gpu::backend {

// The values a pixel of the placeholder integrator has, one after another:
// its color (RGBA), its depth and its normal. A var takes some of them, and
// its other channels are 0.
inline constexpr std::uint32_t kColorValue = 0;
inline constexpr std::uint32_t kDepthValue = 4;
inline constexpr std::uint32_t kNormalValue = 5;
inline constexpr std::uint32_t kPixelValues = 8;

// Where a render var's plane lies in the film, and which of a pixel's values
// it takes.
struct FilmPlane {
  // Its first float in the film.
  std::uint64_t offset = 0;
  std::uint32_t channels = 1;
  // The first of the pixel's values it takes, and how many.
  std::uint32_t first_value = 0;
  std::uint32_t values = 0;
};

// The parameters of a launch, which renders a batch of tiles: one thread per
// pixel of a tile, one tile per layer of the launch.
struct LaunchParams {
  device::CameraModel camera;
  // The IAS; 0 when it is empty.
  OptixTraversableHandle top = 0;
  // The sampler's cache: kSamplerCacheSize bytes.
  CUdeviceptr sampler_cache = 0;
  // The shape of each light, by light id: a device::LightShape each.
  CUdeviceptr light_shapes = 0;
  // The tiles of the launch: a device::PixelWindow each.
  CUdeviceptr tiles = 0;
  // The planes of the film: `plane_count` FilmPlane.
  CUdeviceptr planes = 0;
  // The film: floats, each plane laid out as its render buffer.
  CUdeviceptr film = 0;
  // The data window, which the film covers.
  device::PixelWindow window;
  std::uint32_t plane_count = 0;
  std::uint32_t samples_per_pixel = 1;
};

}  // namespace umbra::gpu::backend

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_GPU_BACKEND_LAUNCH_H_
