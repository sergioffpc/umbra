// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "render.h"

#include <driver_types.h>
#include <optix_types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include "launch.h"
#include "pipeline.h"
#include "support.h"
#include "umbra/device/device.h"
#include "umbra/device/film.h"
#include "umbra/device/gpu/module_abi.h"
#include "umbra/device/image.h"
#include "umbra/device/sampler.h"
#include "umbra/ext/cuda/conversions.h"
#include "umbra/ext/cuda/owners.h"
#include "umbra/ext/cuda/runtime.h"

namespace umbra::device::gpu::backend {

using ext::cuda::DeviceBuffer;

namespace {

// Makes `buffer` hold at least `bytes` bytes, reallocating it when it holds
// fewer.
Result<void> Reserve(DeviceBuffer& buffer, std::size_t bytes) {
  if (buffer.size() >= bytes) {
    return {};
  }
  return DeviceBuffer::Allocate(bytes).transform(
      [&](DeviceBuffer allocated) { buffer = std::move(allocated); });
}

// Uploads a sampler's cache, initialized on the host.
Result<DeviceBuffer> UploadSamplerCache(cudaStream_t stream) {
  std::vector<std::byte> cache(device::kSamplerCacheSize);
  device::InitializeSamplerCache(cache);
  return DeviceBuffer::Upload(cache, stream)
      .and_then([&](DeviceBuffer uploaded) -> Result<DeviceBuffer> {
        return ext::cuda::Synchronize(stream,
                                      "cannot upload the sampler's cache")
            .transform([&] { return std::move(uploaded); });
      });
}

// The film a tile is copied from: its address and the data window it
// covers.
struct FilmSource {
  CUdeviceptr film = 0;
  device::PixelWindow window;
};

// Copies `tile` of `plane` from `source` into `var`'s render buffer, on
// `stream`.
Result<void> CopyTile(const device::FilmVar& var,
                      const device::FilmPlane& plane, const FilmSource& source,
                      const device::PixelWindow& tile, cudaStream_t stream) {
  const device::PixelWindow& window = source.window;
  const std::size_t row_floats =
      static_cast<std::size_t>(window.width()) * plane.channels;
  const std::size_t first =
      device::TileRowOffset(window, tile, tile.y_min, plane.channels);
  const std::size_t count =
      static_cast<std::size_t>(tile.width()) * plane.channels;
  const auto rows = static_cast<std::size_t>(tile.height());
  if (rows == 0 || count == 0) {
    return {};
  }
  if (first + ((rows - 1) * row_floats) + count > var.pixels.size()) {
    return std::unexpected(
        std::format("a render buffer of {} floats is too small for its data "
                    "window",
                    var.pixels.size()));
  }
  const CUdeviceptr from =
      source.film + ((plane.offset + first) * sizeof(float));
  return ext::cuda::Copy2DAsync(
      {
          .to = var.pixels.subspan(first).data(),
          .to_pitch = row_floats * sizeof(float),
          .from = ext::cuda::PointerOf(from),
          .from_pitch = row_floats * sizeof(float),
          .row_bytes = count * sizeof(float),
          .rows = rows,
          .kind = cudaMemcpyDeviceToHost,
      },
      stream, "cannot copy a tile into a render buffer");
}

// Copies `tile` of every plane of `planes` from `source` into the render
// buffer of its var of `vars`, on `stream`.
Result<void> CopyTileOfVars(std::span<const device::FilmVar> vars,
                            std::span<const device::FilmPlane> planes,
                            const FilmSource& source,
                            const device::PixelWindow& tile,
                            cudaStream_t stream) {
  for (const auto& [var, plane] : std::views::zip(vars, planes)) {
    if (auto copied = CopyTile(var, plane, source, tile, stream);
        !copied.has_value()) {
      return copied;
    }
  }
  return {};
}

}  // namespace

Result<void> Renderer::Initialize(OptixDeviceContext context,
                                  cudaStream_t stream) {
  if (!pipeline_.has_value()) {
    auto made = Pipeline::Create(context, stream);
    if (!made.has_value()) {
      return std::unexpected(made.error());
    }
    pipeline_.emplace(*std::move(made));
  }
  if (sampler_cache_.size() == 0) {
    auto uploaded = UploadSamplerCache(stream);
    if (!uploaded.has_value()) {
      return std::unexpected(uploaded.error());
    }
    sampler_cache_ = *std::move(uploaded);
  }
  return {};
}

Result<void> Renderer::Prepare(const device::FrameParameters& frame,
                               const Scene& scene, OptixDeviceContext context,
                               cudaStream_t stream) {
  if (auto initialized = Initialize(context, stream);
      !initialized.has_value()) {
    return initialized;
  }
  const std::optional<device::FilmLayout> layout = device::PlanFilm(frame);
  if (!layout.has_value()) {
    return std::unexpected(std::format("a GPU renders at most {} vars, not {}",
                                       device::kMaxFilmPlanes,
                                       frame.vars.size()));
  }
  layout_ = *layout;
  return Reserve(film_, layout_.floats * sizeof(float))
      .and_then([&] {
        return Reserve(tiles_, kTilesPerLaunch * sizeof(device::PixelWindow));
      })
      .and_then([&] { return Reserve(params_, sizeof(LaunchParams)); })
      .and_then([&] {
        const std::array params = {
            LaunchParams{
                .camera = scene.camera,
                .top = scene.top,
                .sampler_cache = sampler_cache_.address(),
                .light_shapes = scene.light_shapes,
                .tiles = tiles_.address(),
                .film = film_.address(),
                .layout = layout_,
                .window = frame.image.window,
                .samples_per_pixel = frame.samples_per_pixel,
            },
        };
        return params_.Write(std::as_bytes(std::span(params)), 0, stream);
      });
}

Result<void> Renderer::RenderTiles(std::span<const device::PixelWindow> tiles,
                                   cudaStream_t stream) {
  if (!pipeline_.has_value() || tiles.size() > kTilesPerLaunch) {
    return std::unexpected(
        std::format("cannot render {} tiles in one launch{}", tiles.size(),
                    pipeline_.has_value() ? "" : ": no product is prepared"));
  }
  if (tiles.empty()) {
    return {};
  }
  return tiles_.Write(std::as_bytes(tiles), 0, stream).and_then([&] {
    return pipeline_->Launch(params_.address(), device::kTileSize,
                             device::kTileSize,
                             static_cast<std::uint32_t>(tiles.size()), stream);
  });
}

Result<void> Renderer::CopyTiles(const device::FrameParameters& frame,
                                 std::span<const device::PixelWindow> tiles,
                                 cudaStream_t stream) const {
  if (frame.vars.size() != layout_.count) {
    return std::unexpected(std::format("{} vars to copy, but {} prepared",
                                       frame.vars.size(), layout_.count));
  }
  const FilmSource source{
      .film = film_.address(),
      .window = frame.image.window,
  };
  for (const device::PixelWindow& tile : tiles) {
    if (auto copied =
            CopyTileOfVars(frame.vars, layout_.Planes(), source, tile, stream);
        !copied.has_value()) {
      return copied;
    }
  }
  return ext::cuda::Synchronize(
      stream, "cannot render the tiles into the render buffers");
}

}  // namespace umbra::device::gpu::backend
