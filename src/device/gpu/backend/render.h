// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// How a GPU renders a render product with the placeholder integrator, from
// its own memory (docs/adr/0013-devices-render-from-their-own-memory.md):
// its pipeline, its sampler's cache, and a film that covers the product's
// data window, in float32, for each of its render vars. It renders the tiles
// its host thread takes, a batch per launch, into the film, and copies each
// batch into the render buffers once it is rendered
// (docs/adr/0015-tiles-reach-the-render-buffers-as-they-finish.md).

#ifndef UMBRA_DEVICE_GPU_BACKEND_RENDER_H_
#define UMBRA_DEVICE_GPU_BACKEND_RENDER_H_

#include <driver_types.h>
#include <optix_types.h>

#include <optional>
#include <span>
#include <vector>

#include "launch.h"
#include "pipeline.h"
#include "support.h"
#include "umbra/core/safety.h"
#include "umbra/device/camera_rays.h"
#include "umbra/device/device.h"
#include "umbra/device/image.h"
#include "umbra/ext/cuda/owners.h"

UMBRA_ASSUME_NONNULL_BEGIN

namespace umbra::device::gpu::backend {

// What a render product is rendered from, besides its frame parameters.
struct Scene {
  device::CameraModel camera;
  // The IAS; 0 when it is empty.
  OptixTraversableHandle top = 0;
  // The shape of each light, by light id.
  CUdeviceptr light_shapes = 0;
};

// What a GPU renders with. It keeps its pipeline, its sampler's cache and its
// film across render products, and grows the film when a product needs more.
class Renderer {
 public:
  // Prepares the render of the product `frame` describes, of `scene`: makes
  // the pipeline and uploads the sampler's cache, at its first render, and
  // lays out the film for `frame`'s vars.
  [[nodiscard]] Result<void> Prepare(const device::FrameParameters& frame,
                                     const Scene& scene,
                                     OptixDeviceContext context,
                                     cudaStream_t stream);

  // Renders `tiles`, at most kTilesPerLaunch of them, into the film in one
  // launch.
  [[nodiscard]] Result<void> RenderTiles(
      std::span<const device::PixelWindow> tiles, cudaStream_t stream);

  // Copies `tiles` from the film into the render buffers of `frame`'s vars,
  // and waits for the copies.
  [[nodiscard]] Result<void> CopyTiles(
      const device::FrameParameters& frame,
      std::span<const device::PixelWindow> tiles, cudaStream_t stream) const;

 private:
  // Makes the pipeline and uploads the sampler's cache, unless it holds
  // them.
  [[nodiscard]] Result<void> Initialize(OptixDeviceContext context,
                                        cudaStream_t stream);

  std::optional<Pipeline> pipeline_;
  ext::cuda::DeviceBuffer sampler_cache_;
  // The planes of the product prepared, on the host and in the GPU's memory.
  std::vector<FilmPlane> planes_;
  ext::cuda::DeviceBuffer gpu_planes_;
  ext::cuda::DeviceBuffer film_;
  // The tiles of a launch: room for kTilesPerLaunch.
  ext::cuda::DeviceBuffer tiles_;
  ext::cuda::DeviceBuffer params_;
};

}  // namespace umbra::device::gpu::backend

UMBRA_ASSUME_NONNULL_END

#endif  // UMBRA_DEVICE_GPU_BACKEND_RENDER_H_
