// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// What a launch of the placeholder integrator's programs reads, as the host
// writes it and the programs (placeholder.cu) read it: plain data, with the
// GPU's addresses as integers.

#ifndef UMBRA_DEVICE_GPU_BACKEND_LAUNCH_H_
#define UMBRA_DEVICE_GPU_BACKEND_LAUNCH_H_

#include <optix_types.h>

#include <cstdint>

#include "umbra/core/safety.h"
#include "umbra/device/camera_rays.h"
#include "umbra/device/film.h"
#include "umbra/device/image.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device::gpu::backend {

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
  // The film: `layout.floats` floats, each plane laid out as its render
  // buffer.
  CUdeviceptr film = 0;
  device::FilmLayout layout;
  // The data window, which the film covers.
  device::PixelWindow window;
  std::uint32_t samples_per_pixel = 1;
};

}  // namespace umbra::device::gpu::backend

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_GPU_BACKEND_LAUNCH_H_
